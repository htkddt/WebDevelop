# -*- coding: utf-8 -*-
"""
End-to-end check: **initial SharedCollection cold backfill → Elasticsearch** (c-lib storage + elk_sync_pool).

Runs with the normal unittest suite. **Skips** (with a clear reason) if:

- c-lib is missing, or
- Elasticsearch is not reachable (e.g. start ``c-lib/deployments/docker-compose.yml``), or
- MongoDB is not reachable / ``pymongo`` missing, or
- the assertion times out (wrong build: need ``USE_MONGOC=1`` for backfill).

  docker compose -f c-lib/deployments/docker-compose.yml up -d
  cd c-lib && USE_MONGOC=1 make lib
  cd python_ai && python3 -m unittest tests.test_elk_bulk_flow -v

Optional env: ``M4ENGINE_MONGO_URI``, ``M4ENGINE_ES_HOST``, ``M4ENGINE_ES_PORT``, ``M4_ELK_DIAG=1``.

The test writes a tiny SharedCollection registry JSON file (one collection with ``elk.allow: true``),
inserts one Mongo document, calls ``api_create`` (mode MONGO_REDIS_ELK), waits for worker HTTP posts,
then asserts ``GET /idx_<collection>/_count`` ≥ 1.
"""
from __future__ import annotations

import ctypes
import json
import os
import tempfile
import time
import unittest
import urllib.error
import urllib.request
import uuid

_MONGO_URI = (os.environ.get("M4ENGINE_MONGO_URI") or "mongodb://127.0.0.1:27017").strip()
# DB name passed via ApiOptions.shared_collection_backfill_db only (no SC-specific env).
_TEST_BACKFILL_DB = "m4_ai"


def _es_base_url() -> str:
    h = (os.environ.get("M4ENGINE_ES_HOST") or "127.0.0.1").strip() or "127.0.0.1"
    try:
        p = int(os.environ.get("M4ENGINE_ES_PORT") or "9200")
    except ValueError:
        p = 9200
    if p <= 0:
        p = 9200
    return f"http://{h}:{p}"


def _es_reachable() -> bool:
    try:
        with urllib.request.urlopen(_es_base_url() + "/", timeout=3) as resp:
            return 200 <= resp.status < 300
    except (urllib.error.URLError, OSError, TimeoutError, ValueError):
        return False


def _es_index_count(index: str) -> int:
    url = f"{_es_base_url()}/{index}/_count"
    with urllib.request.urlopen(url, timeout=10) as resp:
        body = resp.read().decode("utf-8", errors="replace")
    data = json.loads(body)
    return int(data.get("count", 0))


class TestElkBulkFlow(unittest.TestCase):
    """Cold backfill on api_create → document visible in Elasticsearch."""

    @classmethod
    def setUpClass(cls):
        try:
            from engine_ctypes import load_lib  # noqa: PLC0415

            cls._lib = load_lib()
        except (FileNotFoundError, OSError):
            cls._lib = None

    def setUp(self):
        self._lib = self.__class__._lib
        if self._lib is None:
            self.skipTest("libm4engine not found (python_ai/lib or libs; USE_MONGOC=1 build for this test)")
        if not _es_reachable():
            self.skipTest(f"Elasticsearch not reachable at {_es_base_url()} (start deployments/docker-compose)")
        try:
            from pymongo import MongoClient  # noqa: PLC0415
        except ImportError:
            self.skipTest("pymongo not installed")

        try:
            self._mongo = MongoClient(_MONGO_URI, serverSelectionTimeoutMS=3000)
            self._mongo.admin.command("ping")
        except Exception as ex:
            self.skipTest(f"MongoDB not reachable at {_MONGO_URI!r}: {ex}")

        self._coll_name = f"elk_bulk_{uuid.uuid4().hex[:10]}"
        self._index_name = f"idx_{self._coll_name}"
        self._json_path = None

    def tearDown(self):
        mongo = getattr(self, "_mongo", None)
        coll_name = getattr(self, "_coll_name", None)
        if mongo and coll_name:
            try:
                mongo[_TEST_BACKFILL_DB][coll_name].drop()
            except Exception:
                pass
            try:
                mongo.close()
            except Exception:
                pass
        if self._json_path and os.path.isfile(self._json_path):
            try:
                os.remove(self._json_path)
            except OSError:
                pass

    def test_initial_bulk_creates_index_with_one_doc(self):
        from engine_ctypes import ApiOptions, M4ENGINE_MODE_MONGO_REDIS_ELK  # noqa: PLC0415

        spec = {
            "collections": [
                {
                    "collection": self._coll_name,
                    "public": ["title"],
                    "sensitive": [],
                    "elk": {"allow": True, "index": ".", "transform": False},
                }
            ]
        }
        fd, self._json_path = tempfile.mkstemp(suffix=".json", prefix="m4_sc_elk_")
        os.close(fd)
        with open(self._json_path, "w", encoding="utf-8") as f:
            json.dump(spec, f)

        # Ensure pool + backfill (defaults); do not inherit accidental disables from shell
        os.environ.pop("M4_ELK_SYNC_POOL", None)
        os.environ.pop("M4_ELK_BACKFILL", None)

        self._mongo[_TEST_BACKFILL_DB][self._coll_name].insert_one(
            {"title": "ELK bulk flow probe", "probe": True}
        )

        es_h = (os.environ.get("M4ENGINE_ES_HOST") or "127.0.0.1").strip() or "127.0.0.1"
        try:
            es_p = int(os.environ.get("M4ENGINE_ES_PORT") or "9200")
        except ValueError:
            es_p = 9200
        if es_p <= 0:
            es_p = 9200

        opts = ApiOptions(
            mode=M4ENGINE_MODE_MONGO_REDIS_ELK,
            mongo_uri=_MONGO_URI.encode("utf-8"),
            redis_host=b"127.0.0.1",
            redis_port=6379,
            es_host=es_h.encode("utf-8"),
            es_port=es_p,
            log_db=None,
            log_coll=None,
            context_batch_size=0,
            smart_topic_opts=None,
            inject_geo_knowledge=0,
            disable_auto_system_time=0,
            geo_authority=0,
            model_switch_opts=None,
            vector_gen_backend=0,
            vector_ollama_model=None,
            embed_migration_autostart=0,
            session_idle_seconds=0,
            shared_collection_mongo_uri=_MONGO_URI.encode("utf-8"),
            shared_collection_json_path=self._json_path.encode("utf-8"),
            shared_collection_backfill_db=_TEST_BACKFILL_DB.encode("utf-8"),
            learning_terms_path=None,
            enable_learning_terms=0,
            defer_learning_terms_load=0,
        )

        ctx = self._lib.api_create(ctypes.byref(opts))
        self.assertIsNotNone(ctx, "api_create(MONGO_REDIS_ELK) failed")
        try:
            deadline = time.time() + 30.0
            count = 0
            while time.time() < deadline:
                try:
                    count = _es_index_count(self._index_name)
                    if count >= 1:
                        break
                except (urllib.error.HTTPError, urllib.error.URLError, OSError, ValueError, json.JSONDecodeError):
                    pass
                time.sleep(0.5)

            self.assertGreaterEqual(
                count,
                1,
                f"expected >=1 doc in ES index {self._index_name!r} after cold backfill; got {count}. "
                f"Check stderr for [ELK] or [STORAGE][ELK_DIAG] (set M4_ELK_DIAG=1). "
                f"Confirm c-lib built with USE_MONGOC=1.",
            )
        finally:
            self._lib.api_destroy(ctx)


if __name__ == "__main__":
    unittest.main()
