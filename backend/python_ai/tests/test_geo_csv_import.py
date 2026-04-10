# -*- coding: utf-8 -*-
"""
Geo CSV import regression tests (uses docs/csv/geo.csv).

Embeddings for ``embed=1`` are computed in c-lib via ``api_embed_text`` (not Python HTTP).

Run (from ``python_ai/``)::

  python3 -m unittest tests.test_geo_csv_import -v
"""
from __future__ import annotations

import os
import sys
import unittest

# python_ai on path → ``import server.geo_csv_import``
_PYTHON_AI = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _PYTHON_AI not in sys.path:
    sys.path.insert(0, _PYTHON_AI)

from server import geo_csv_import as geo  # noqa: E402


def _repo_geo_csv_path() -> str:
    """``python_ai/tests/`` → repo root ``docs/csv/geo.csv``."""
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.normpath(os.path.join(here, "..", "..", "docs", "csv", "geo.csv"))


class TestGeoCsvSampleFile(unittest.TestCase):
    """Parse the real checked-in ``docs/csv/geo.csv`` (location_name-style headers)."""

    def test_parse_geo_csv_non_empty(self) -> None:
        path = _repo_geo_csv_path()
        self.assertTrue(os.path.isfile(path), f"missing sample CSV: {path}")
        with open(path, encoding="utf-8") as f:
            text = f.read()
        rows, errs = geo.parse_geo_csv(text)
        self.assertEqual(errs, [], f"unexpected parse_errors: {errs}")
        self.assertGreaterEqual(len(rows), 30, "expected ~33 Vietnamese locations")
        self.assertEqual(rows[0].get("name"), "Bắc Ninh")
        self.assertIn("Chùa Dâu", rows[0].get("embed_text") or "")


class TestEmbedModelName(unittest.TestCase):
    def test_embed_model_name_env_chain(self) -> None:
        old_e = os.environ.pop("OLLAMA_EMBED_MODEL", None)
        old_m = os.environ.pop("OLLAMA_MODEL", None)
        try:
            os.environ["OLLAMA_EMBED_MODEL"] = "nomic-embed-text"
            self.assertEqual(geo.embed_model_name(), "nomic-embed-text")
            del os.environ["OLLAMA_EMBED_MODEL"]
            os.environ["OLLAMA_MODEL"] = "llama3.2:1b"
            self.assertEqual(geo.embed_model_name(), "llama3.2:1b")
        finally:
            if old_e is not None:
                os.environ["OLLAMA_EMBED_MODEL"] = old_e
            else:
                os.environ.pop("OLLAMA_EMBED_MODEL", None)
            if old_m is not None:
                os.environ["OLLAMA_MODEL"] = old_m
            else:
                os.environ.pop("OLLAMA_MODEL", None)


if __name__ == "__main__":
    unittest.main()
