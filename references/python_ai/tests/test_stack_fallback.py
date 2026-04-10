# -*- coding: utf-8 -*-
from __future__ import annotations

import os
import sys
import unittest

_PYTHON_AI = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _PYTHON_AI not in sys.path:
    sys.path.insert(0, _PYTHON_AI)


class TestApplyStackFallback(unittest.TestCase):
    def test_elk_down_3_to_2(self):
        from training.full_options import M4ENGINE_MODE_MONGO_REDIS_ELK, apply_stack_fallback

        d = {
            "mode": M4ENGINE_MODE_MONGO_REDIS_ELK,
            "mongo_uri": "mongodb://127.0.0.1:27017",
            "redis_host": "127.0.0.1",
            "redis_port": 6379,
            "es_host": "127.0.0.1",
            "es_port": 9200,
            "shared_collection_json_path": "/tmp/sc_registry.json",
            "shared_collection_backfill_db": "product",
            "shared_collection_mongo_uri": "mongodb://127.0.0.1:27017",
        }
        out, reasons = apply_stack_fallback(d, mongo_ok=True, redis_ok=True, elk_ok=False)
        self.assertEqual(out["mode"], 2)
        self.assertIsNone(out.get("es_host"))
        self.assertIsNone(out.get("shared_collection_json_path"))
        self.assertIsNone(out.get("shared_collection_mongo_uri"))
        self.assertIsNone(out.get("shared_collection_backfill_db"))
        self.assertIn("Elasticsearch", reasons[0])

    def test_redis_down_3_to_1(self):
        from training.full_options import M4ENGINE_MODE_MONGO_REDIS_ELK, apply_stack_fallback

        d = {
            "mode": M4ENGINE_MODE_MONGO_REDIS_ELK,
            "mongo_uri": "mongodb://127.0.0.1:27017",
            "redis_host": "127.0.0.1",
            "redis_port": 6379,
            "es_host": "127.0.0.1",
            "es_port": 9200,
        }
        out, reasons = apply_stack_fallback(d, mongo_ok=True, redis_ok=False, elk_ok=True)
        self.assertEqual(out["mode"], 1)
        self.assertIsNone(out.get("redis_host"))
        self.assertIn("Redis", reasons[0])

    def test_mongo_down_to_memory(self):
        from training.full_options import M4ENGINE_MODE_MONGO_REDIS_ELK, apply_stack_fallback

        d = {
            "mode": M4ENGINE_MODE_MONGO_REDIS_ELK,
            "mongo_uri": "mongodb://127.0.0.1:27017",
            "redis_host": "127.0.0.1",
            "redis_port": 6379,
            "es_host": "127.0.0.1",
            "es_port": 9200,
        }
        out, reasons = apply_stack_fallback(d, mongo_ok=False, redis_ok=False, elk_ok=False)
        self.assertEqual(out["mode"], 0)
        self.assertIsNone(out.get("mongo_uri"))
        self.assertIn("Mongo", reasons[0])


class TestValidateClientBotCLib(unittest.TestCase):
    def test_rejects_unknown_key(self):
        from training.full_options import validate_client_bot_c_lib_values

        with self.assertRaises(ValueError) as cx:
            validate_client_bot_c_lib_values({"inject_geo_knowledge": 1})
        self.assertIn("unknown key", str(cx.exception))


class TestResolveEngineOptionsDict(unittest.TestCase):
    def test_explicit_connectivity_skips_probe(self):
        from training.full_options import get_max_options, resolve_engine_options_dict

        base = get_max_options()
        eff, reasons = resolve_engine_options_dict(
            True,
            {},
            apply_fallback=True,
            connectivity={"mongo_ok": False, "redis_ok": False, "elk_ok": False},
        )
        self.assertEqual(eff["mode"], 0)
        self.assertTrue(reasons)


if __name__ == "__main__":
    unittest.main()
