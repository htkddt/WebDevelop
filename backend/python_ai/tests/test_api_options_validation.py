# -*- coding: utf-8 -*-
"""Strict ``validate_api_options_dict`` — guards Python → c-lib ``api_create`` contract."""
from __future__ import annotations

import os
import sys
import unittest

_PYTHON_AI = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _PYTHON_AI not in sys.path:
    sys.path.insert(0, _PYTHON_AI)


class TestValidateApiOptionsDict(unittest.TestCase):
    def test_max_snapshot_passes(self):
        from training.full_options import build_snapshot_bot_c_lib_values_for_db, validate_api_options_dict

        validate_api_options_dict(build_snapshot_bot_c_lib_values_for_db(use_max=True))

    def test_rejects_mode_out_of_range(self):
        from training.full_options import get_max_options, validate_api_options_dict

        d = dict(get_max_options())
        d["mode"] = 4
        with self.assertRaises(ValueError) as cx:
            validate_api_options_dict(d)
        self.assertIn("0..3", str(cx.exception))

    def test_rejects_mode_bool(self):
        from training.full_options import get_max_options, validate_api_options_dict

        d = dict(get_max_options())
        d["mode"] = True
        with self.assertRaises(ValueError) as cx:
            validate_api_options_dict(d)
        self.assertIn("boolean", str(cx.exception))

    def test_rejects_context_batch_over_clib_max(self):
        from training.full_options import _CLIB_CONTEXT_BATCH_MAX, get_max_options, validate_api_options_dict

        d = dict(get_max_options())
        d["context_batch_size"] = _CLIB_CONTEXT_BATCH_MAX + 1
        with self.assertRaises(ValueError) as cx:
            validate_api_options_dict(d)
        self.assertIn("API_CTX_CAPACITY_MAX", str(cx.exception))

    def test_rejects_nul_in_mongo_uri(self):
        from training.full_options import get_max_options, validate_api_options_dict

        d = dict(get_max_options())
        d["mongo_uri"] = "mongodb://x\x00bad"
        with self.assertRaises(ValueError) as cx:
            validate_api_options_dict(d)
        self.assertIn("NUL", str(cx.exception))

    def test_rejects_nul_in_learning_terms_path(self):
        from training.full_options import get_max_options, validate_api_options_dict

        d = dict(get_max_options())
        d["learning_terms_path"] = "/tmp/a\x00b.tsv"
        with self.assertRaises(ValueError) as cx:
            validate_api_options_dict(d)
        self.assertIn("NUL", str(cx.exception))

    def test_rejects_invalid_shared_collection_mongo_uri(self):
        from training.full_options import get_max_options, validate_api_options_dict

        d = dict(get_max_options())
        d["shared_collection_mongo_uri"] = "localhost"
        with self.assertRaises(ValueError) as cx:
            validate_api_options_dict(d)
        self.assertIn("shared_collection_mongo_uri", str(cx.exception))

    def test_shared_collection_inject_fills_store_defaults(self):
        from training.full_options import get_max_options, validate_api_options_dict

        d = get_max_options()
        self.assertTrue((d.get("shared_collection_json_path") or "").strip())
        self.assertEqual(d.get("shared_collection_backfill_db"), "product")
        self.assertTrue((d.get("shared_collection_mongo_uri") or "").startswith("mongodb://"))
        validate_api_options_dict(d)

    def test_es_host_requires_mode_3_after_coerce_ok(self):
        from training.full_options import (
            M4ENGINE_MODE_MONGO_REDIS,
            _build_api_options_from_dict,
            get_full_options,
            merge_options_overrides,
        )

        base = dict(get_full_options())
        base["mode"] = M4ENGINE_MODE_MONGO_REDIS
        base["es_host"] = "127.0.0.1"
        o, _, _ = _build_api_options_from_dict(merge_options_overrides(base, {}))
        self.assertEqual(o.mode, 3)
        self.assertEqual(o.es_host, b"127.0.0.1")


if __name__ == "__main__":
    unittest.main()
