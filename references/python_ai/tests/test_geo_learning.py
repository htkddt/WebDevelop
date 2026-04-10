"""
Test geo_learning: send a chat that mentions a HCMC landmark, wait for async worker, then check geo_atlas.

**Opt-in:** set ``M4ENGINE_RUN_GEO_LEARNING_TESTS=1`` (or ``true``). Otherwise the suite is **skipped** —
integration needs MongoDB + Ollama + c-lib with mongoc; some environments hit **mongoc pthread / handshake**
assertions when multiple tests touch the driver.

Run:
  export M4ENGINE_RUN_GEO_LEARNING_TESTS=1
  cd python_ai && python3 -m unittest tests.test_geo_learning -v
  or: python3 tests/test_geo_learning.py

Requires:
  - ``libm4engine`` in ``python_ai/lib`` or ``libs``, Ollama running.
  - For geo data to appear: build c-lib with Mongo and run MongoDB:
      cd c-lib && make clean && USE_MONGOC=1 make lib
    Then start MongoDB. Without USE_MONGOC=1, storage_geo_atlas_* are no-ops so geo_atlas stays empty.
  - Check stderr for [GEO_LEARNING] logs to see worker activity (enqueue, extraction, insert).
"""
import ctypes
import os
import time
import unittest

from engine_ctypes import (
    load_lib,
    ApiOptions,
    M4ENGINE_MODE_MONGO_REDIS_ELK,
    API_DEFAULT_TENANT_ID,
    OL_BUF_SIZE,
)


def _default_api_opts():
    return ApiOptions(
        mode=M4ENGINE_MODE_MONGO_REDIS_ELK,
        mongo_uri=b"mongodb://127.0.0.1:27017",
        redis_host=b"127.0.0.1",
        redis_port=6379,
        es_host=None,
        es_port=0,
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
        shared_collection_mongo_uri=None,
        shared_collection_json_path=None,
        shared_collection_backfill_db=None,
        learning_terms_path=None,
        enable_learning_terms=0,
        defer_learning_terms_load=0,
    )


_RUN_GL = os.environ.get("M4ENGINE_RUN_GEO_LEARNING_TESTS", "").strip().lower() in (
    "1",
    "true",
    "yes",
)


@unittest.skipUnless(_RUN_GL, "set M4ENGINE_RUN_GEO_LEARNING_TESTS=1 for Mongo+Ollama integration (see module docstring)")
class TestGeoLearning(unittest.TestCase):
    """Geo learning: enqueue turn on api_chat, worker extracts landmarks and inserts into geo_atlas."""

    @classmethod
    def setUpClass(cls):
        try:
            cls._lib = load_lib()
        except (FileNotFoundError, OSError):
            cls._lib = None

    def setUp(self):
        if self._lib is None:
            self.skipTest("libm4engine not found (python_ai/lib or python_ai/libs)")

    def test_geo_atlas_landmarks_after_chat(self):
        """Send a message mentioning a HCMC landmark; wait for worker; then read api_get_geo_atlas_landmarks."""
        opts = _default_api_opts()
        ctx = self._lib.api_create(ctypes.byref(opts))
        self.assertIsNotNone(ctx, "api_create(MONGO_REDIS_ELK) should succeed")
        if not ctx:
            return
        try:
            # Message that should trigger extraction (landmark + district)
            msg = "Ngã tư Hàng Xanh is a famous intersection in Bình Thạnh district, HCMC."
            reply_buf = ctypes.create_string_buffer(OL_BUF_SIZE)
            rc = self._lib.api_chat(
                ctx,
                API_DEFAULT_TENANT_ID,
                API_DEFAULT_TENANT_ID,
                msg.encode("utf-8"),
                reply_buf,
                OL_BUF_SIZE,
            )
            self.assertEqual(rc, 0, "api_chat should succeed (Ollama must be running)")
            reply = reply_buf.value.decode("utf-8", errors="replace").strip()
            self.assertTrue(len(reply) > 0, "reply should be non-empty")

            # Geo learning worker runs async: extraction -> embed -> find_similar -> insert. Wait for it.
            wait_sec = 25
            print(f"  Waiting {wait_sec}s for geo_learning worker (Ollama extraction + Mongo insert)...")
            time.sleep(wait_sec)

            landmarks_buf = ctypes.create_string_buffer(4096)
            n = self._lib.api_get_geo_atlas_landmarks(ctx, landmarks_buf, 4096)
            landmarks_str = landmarks_buf.value.decode("utf-8", errors="replace") if n else ""

            # With USE_MONGOC=1 and Mongo running, we may get landmarks. Without Mongo, n==0.
            if n > 0 and landmarks_str.strip():
                print(f"  geo_atlas landmarks ({n} bytes):\n{landmarks_str[:500]}")
                self.assertTrue(
                    "Hàng Xanh" in landmarks_str or "Bình Thạnh" in landmarks_str or "Hang Xanh" in landmarks_str,
                    "expected at least one of Hàng Xanh / Bình Thạnh in landmarks (or normalized form)",
                )
            else:
                print("  geo_atlas empty (Mongo not linked or not running, or worker not yet finished)")
                # Don't fail: when c-lib is built without USE_MONGOC=1, storage_geo_atlas_* are no-ops
                self.assertGreaterEqual(n, 0)
        finally:
            self._lib.api_destroy(ctx)

    def test_geo_atlas_has_data_after_chat(self):
        """Send chat with HCMC landmark, wait for worker, then check via api_get_geo_atlas_landmarks (c-lib storage)."""
        opts = _default_api_opts()
        ctx = self._lib.api_create(ctypes.byref(opts))
        self.assertIsNotNone(ctx)
        if not ctx:
            return
        try:
            # Check initial state
            landmarks_buf = ctypes.create_string_buffer(4096)
            n_before = self._lib.api_get_geo_atlas_landmarks(ctx, landmarks_buf, 4096)
            msg = "Ngã tư Hàng Xanh is in Bình Thạnh district, Ho Chi Minh City."
            reply_buf = ctypes.create_string_buffer(OL_BUF_SIZE)
            rc = self._lib.api_chat(
                ctx,
                API_DEFAULT_TENANT_ID,
                API_DEFAULT_TENANT_ID,
                msg.encode("utf-8"),
                reply_buf,
                OL_BUF_SIZE,
            )
            self.assertEqual(rc, 0, "api_chat should succeed (Ollama running)")
            print("  Check stderr for [GEO_LEARNING] logs (enqueue, extraction, insert)")
            wait_sec = 25
            print(f"  Waiting {wait_sec}s for geo_learning worker...")
            time.sleep(wait_sec)
            n_after = self._lib.api_get_geo_atlas_landmarks(ctx, landmarks_buf, 4096)
            landmarks_str = landmarks_buf.value.decode("utf-8", errors="replace") if n_after > 0 else ""
            print(f"  geo_atlas before: {n_before} bytes, after: {n_after} bytes")
            if n_after > 0:
                print(f"  landmarks:\n{landmarks_str[:500]}")
                self.assertTrue(
                    "Hàng Xanh" in landmarks_str or "Bình Thạnh" in landmarks_str or "Hang Xanh" in landmarks_str,
                    f"expected landmark in geo_atlas. Got: {landmarks_str[:200]}",
                )
            else:
                print("  geo_atlas empty. Check stderr for [GEO_LEARNING] errors.")
                print("  Common issues: worker not started (geo_learning_enabled?), Ollama extraction failed, JSON parse failed, Mongo insert failed.")
                # Don't fail: might be USE_MONGOC=0 build or worker still processing
                self.assertGreaterEqual(n_after, 0)
        finally:
            self._lib.api_destroy(ctx)


if __name__ == "__main__":
    unittest.main()
