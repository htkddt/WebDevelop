# -*- coding: utf-8 -*-
"""
Bot c-lib DB merge helpers, SharedCollection catalog, and ``GET /api/meta/shared-collections``.

Run from ``python_ai/``:
  python3 -m unittest tests.test_bot_c_lib_shared_collection -v

HTTP slice is skipped when ``M4_APP_MONGO_DISABLE=1`` (same as other app Mongo tests).
"""
from __future__ import annotations

import json
import os
import sys
import unittest
from unittest.mock import patch

_PYTHON_AI = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _PYTHON_AI not in sys.path:
    sys.path.insert(0, _PYTHON_AI)
# ``server/*.py`` uses ``from app_mongo import …`` (expects ``server/`` on path).
_SERVER_DIR = os.path.join(_PYTHON_AI, "server")
if _SERVER_DIR not in sys.path:
    sys.path.insert(0, _SERVER_DIR)


def _app_mongo_disabled() -> bool:
    return os.environ.get("M4_APP_MONGO_DISABLE", "").strip().lower() in ("1", "true", "yes")


class TestFullOptionsSnapshot(unittest.TestCase):
    """``build_snapshot_bot_c_lib_values_for_db`` + merge → ``ApiOptions`` (no Flask)."""

    def test_snapshot_keys_are_subset_of_option_keys(self):
        from training.full_options import OPTION_KEYS, build_snapshot_bot_c_lib_values_for_db

        snap = build_snapshot_bot_c_lib_values_for_db(use_max=True)
        for k in snap:
            self.assertIn(k, OPTION_KEYS, f"unexpected key {k!r}")

    def test_snapshot_use_full_vs_max_mode(self):
        from training.full_options import build_snapshot_bot_c_lib_values_for_db

        a = build_snapshot_bot_c_lib_values_for_db(use_max=True)
        b = build_snapshot_bot_c_lib_values_for_db(use_max=False)
        self.assertIsInstance(a, dict)
        self.assertIsInstance(b, dict)
        self.assertIn("mode", a)
        self.assertIn("mode", b)

    def test_merge_overrides_into_max_base(self):
        from training.full_options import (
            build_max_api_options_with_overrides,
            get_max_options,
            merge_options_overrides,
        )

        base = get_max_options()
        merged = merge_options_overrides(base, {"redis_port": 6380})
        self.assertEqual(merged.get("redis_port"), 6380)
        opts, hist, _st = build_max_api_options_with_overrides({"redis_port": 6380})
        self.assertEqual(opts.redis_port, 6380)
        if merged.get("context_batch_size"):
            self.assertEqual(hist, int(merged["context_batch_size"]))
        else:
            self.assertGreater(hist, 0)


class TestSharedCollectionCatalog(unittest.TestCase):
    """Static catalog + JSON export (no Mongo)."""

    def test_three_store_collections(self):
        import shared_collection_catalog as cat

        names = {x["collection"] for x in cat.DEFAULT_STORE_SHARED_COLLECTIONS}
        self.assertEqual(names, {"products", "carts", "product_categories"})

    def test_products_public_whitelist(self):
        import shared_collection_catalog as cat

        prods = next(x for x in cat.DEFAULT_STORE_SHARED_COLLECTIONS if x["collection"] == "products")
        self.assertIn("sku", prods["public"])
        self.assertIn("sensitive", prods)
        self.assertIn("metadata", prods)

    def test_carts_has_join_hint(self):
        import shared_collection_catalog as cat

        cart = next(x for x in cat.DEFAULT_STORE_SHARED_COLLECTIONS if x["collection"] == "carts")
        self.assertIn("lines", cart["public"])
        joins = cart.get("joins") or []
        self.assertTrue(joins)
        self.assertEqual(joins[0].get("to_collection"), "products")

    def test_catalog_json_roundtrip(self):
        import shared_collection_catalog as cat

        raw = cat.catalog_json_bytes()
        data = json.loads(raw.decode("utf-8"))
        self.assertIn("collections", data)
        self.assertEqual(len(data["collections"]), 3)

    def test_catalog_copy_is_deep(self):
        import shared_collection_catalog as cat

        c = cat.catalog_collections_copy()
        c[0]["alias"] = "mutated_for_test"
        self.assertNotEqual(cat.DEFAULT_STORE_SHARED_COLLECTIONS[0]["alias"], c[0]["alias"])

    def test_all_shared_collections_elk_allow_true_in_defaults(self):
        import shared_collection_catalog as cat

        for row in cat.DEFAULT_STORE_SHARED_COLLECTIONS:
            self.assertTrue(row["elk"]["allow"], msg=row.get("collection"))


class TestSharedCollectionMetaWithoutMongo(unittest.TestCase):
    """``list_shared_collections_meta`` code path."""

    def test_source_code_when_mongo_disabled_flag(self):
        import shared_collection_store as sc

        with patch.object(sc, "app_mongo_disabled", return_value=True):
            with patch(
                "shared_collection_catalog.shared_collections_store_json_path",
                return_value="/__no_shared_collections_store__.json",
            ):
                meta = sc.list_shared_collections_meta()
        self.assertEqual(meta["source"], "code")
        self.assertEqual(len(meta["collections"]), 3)
        self.assertIn("spec", meta)


class TestBotCibSettingsMemory(unittest.TestCase):
    """In-memory ``bot_c_lib_settings`` with ``identity_persists_mongo`` forced False."""

    def setUp(self):
        import bot_c_lib_settings_store as b

        self._b = b
        self._patch = patch.object(b, "identity_persists_mongo", return_value=False)
        self._patch.start()
        b._memory_values.clear()

    def tearDown(self):
        self._patch.stop()
        self._b._memory_values.clear()

    def test_ensure_full_creates_once(self):
        b = self._b
        self.assertTrue(b.ensure_bot_c_lib_full_if_no_document())
        self.assertFalse(b.ensure_bot_c_lib_full_if_no_document())
        got = b.get_bot_c_lib_overrides()
        self.assertGreater(len(got), 0)
        self.assertIn("mode", got)

    def test_save_replace_merges_into_api_options(self):
        from training.full_options import build_max_api_options_with_overrides

        b = self._b
        b.save_bot_c_lib_settings(
            {"context_batch_size": 41},
            replace=True,
            updated_by="test",
        )
        opts, hist, _k = build_max_api_options_with_overrides(b.get_bot_c_lib_overrides())
        self.assertEqual(opts.context_batch_size, 41)
        self.assertEqual(hist, 41)

    def test_normalize_drops_empty_string_values(self):
        b = self._b
        b.save_bot_c_lib_settings(
            {"shared_collection_mongo_uri": "", "redis_port": 6380},
            replace=True,
            updated_by="test",
        )
        got = b.get_bot_c_lib_overrides()
        self.assertNotIn("shared_collection_mongo_uri", got)
        self.assertEqual(got.get("redis_port"), 6380)


def _server_app_importable() -> bool:
    try:
        import app as srv  # noqa: F401

        return True
    except ImportError:
        return False


@unittest.skipIf(_app_mongo_disabled(), "M4_APP_MONGO_DISABLE — skip meta HTTP + Mongo-backed checks")
@unittest.skipUnless(_server_app_importable(), "server app import failed (e.g. missing pymongo/bson)")
class TestMetaSharedCollectionsHTTP(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import app as srv

        cls._srv = srv
        srv._app.config["TESTING"] = True
        cls.client = srv._app.test_client()

    def test_meta_shared_collections_ok(self):
        r = self.client.get("/api/meta/shared-collections")
        self.assertEqual(r.status_code, 200, r.get_json())
        data = r.get_json()
        self.assertIn("collections", data)
        self.assertIn("source", data)
        names = {c["collection"] for c in data["collections"]}
        self.assertEqual(names, {"products", "carts", "product_categories"})
        self.assertIn(data["source"], ("mongo", "mongo+store", "code", "store", "code_fallback"))
        for row in data["collections"]:
            self.assertIn("public", row)
            self.assertIn("alias", row)

    def test_shared_collections_mongo_seeded_when_reachable(self):
        """After app import, ``shared_collections`` should be non-empty if DB works."""
        from app_mongo import app_mongo_disabled, get_app_database

        if app_mongo_disabled():
            self.skipTest("app mongo disabled")
        try:
            n = get_app_database()["shared_collections"].estimated_document_count()
        except Exception as exc:
            self.skipTest(f"app db unavailable: {exc}")
        self.assertGreaterEqual(
            n,
            1,
            "expected ensure_shared_collections_seeded() to have run at app import",
        )


if __name__ == "__main__":
    unittest.main()
