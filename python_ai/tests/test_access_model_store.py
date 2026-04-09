# -*- coding: utf-8 -*-
"""``access_pages`` Mongo seed + ``GET /api/meta/access-model`` shape."""
from __future__ import annotations

import os
import sys
import unittest

_PYTHON_AI = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _PYTHON_AI not in sys.path:
    sys.path.insert(0, _PYTHON_AI)


def _app_mongo_disabled() -> bool:
    return os.environ.get("M4_APP_MONGO_DISABLE", "").strip().lower() in ("1", "true", "yes")


@unittest.skipIf(_app_mongo_disabled(), "M4_APP_MONGO_DISABLE")
class TestAccessModelStore(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import server.app as srv

        cls._srv = srv
        srv._app.config["TESTING"] = True
        cls.client = srv._app.test_client()

    def test_meta_access_model_web_slice(self):
        r = self.client.get("/api/meta/access-model")
        self.assertEqual(r.status_code, 200, r.get_json())
        data = r.get_json()
        self.assertEqual(data.get("platform"), "WEB")
        self.assertIn("webPages", data)
        self.assertNotIn("adminDeskPages", data)
        self.assertIn(data.get("source", ""), ("mongodb", "static", "static_error"))
        self.assertEqual(data.get("catalog_source"), data.get("source"))
        paths = {wp.get("path") for wp in (data.get("webPages") or [])}
        self.assertIn("/productions", paths)
        self.assertIn("/cart", paths)
        for wp in data.get("webPages") or []:
            self.assertIn("audiences", wp)
            self.assertIsInstance(wp["audiences"], list)
            self.assertNotIn("mainNavAudiences", wp)
        nav = data.get("webNav") or []
        self.assertIsInstance(nav, list)
        self.assertGreaterEqual(len(nav), 1)
        for item in nav:
            self.assertIn("name", item)
            self.assertIn("link", item)
            self.assertIn("nav", item)
        menu = data.get("webMenu") or []
        self.assertIsInstance(menu, list)

    def test_meta_access_model_admin_slice(self):
        r = self.client.get("/api/meta/access-model?platform=admin")
        self.assertEqual(r.status_code, 200, r.get_json())
        data = r.get_json()
        self.assertEqual(data.get("platform"), "ADMIN")
        self.assertIn("adminDeskPages", data)
        self.assertIn("adminBotPage", data)
        self.assertIsInstance(data.get("staffShellPaths"), list)

    def test_meta_access_model_all(self):
        r = self.client.get("/api/meta/access-model?platform=all")
        self.assertEqual(r.status_code, 200, r.get_json())
        data = r.get_json()
        self.assertIn("webPages", data)
        self.assertIn("adminDeskPages", data)
        self.assertIn("webPagesByPlatform", data)

    def test_unknown_platform_404(self):
        r = self.client.get("/api/meta/access-model?platform=zz_unknown_platform_xyz")
        self.assertEqual(r.status_code, 404, r.get_json())
        data = r.get_json()
        self.assertEqual(data.get("error"), "unknown_platform")
        self.assertIsInstance(data.get("known_platforms"), list)
