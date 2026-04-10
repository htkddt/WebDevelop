# -*- coding: utf-8 -*-
"""
HTTP tests for python_ai/server/app.py (Flask on :5000).

Why history / stats look "empty" in the UI (normal cases)
--------------------------------------------------------
1. **History `messages: []`** — Fresh engine: no `api_chat` yet, and `api_load_chat_history`
   only fills from Mongo when Mongo is connected and has rows. In MEMORY mode, history
   stays empty until you send at least one chat.
2. **Stats mostly zeros / false** — `processed`, `errors`, counters start at 0; `mongo_connected`
   is false in MEMORY mode; `ollama_connected` may be 0 until a health check runs (depends
   on c-lib). This is expected, not a broken API.
3. **Frontend shows "—"** — If `fetch` fails (wrong `VITE_API_URL`, server down, CORS),
   the FE catches and puts the error string in the panel; fix URL and run server on :5000.

Chat turns in Mongo (`bot.records`)
-----------------------------------
- Requires **c-lib built with `USE_MONGOC=1`**, valid **`M4ENGINE_MONGO_URI`**, and mode
  not **ONLY_MEMORY**. Otherwise `engine_append_turn` does not insert (see c-lib stderr).
- **Non-stream / stream:** c-lib persists after Ollama returns; if the response body cannot
  be parsed into text, the turn may still be stored with **empty `turn.assistant`** (audit)
  rather than skipping the insert entirely.

Run (from python_ai/):

- Prefer the **server venv** so ``import server.app`` finds ``pymongo``/``bson``
  (``product_cart_store``): ``../server/.venv/bin/python -m unittest tests.test_server -v``
- Or: ``pip install -r server/requirements.txt`` into the interpreter you use for tests.

Plain:

  python3 -m unittest tests.test_server -v

Optional live Ollama:
  M4ENGINE_TEST_SERVER_CHAT=1       — `POST /api/chat`
  M4ENGINE_TEST_SERVER_CHAT_STREAM=1 — `POST /api/chat/stream` (full SSE read + processed++)

When **`stats.mongo_connected`** is true after the stream test, the test **queries MongoDB**
(`bot.records`) for `temp_message_id` and asserts **`turn.input`**. Install **`pip install -r requirements-dev.txt`**
(or `pymongo` alone). URI: **`M4ENGINE_MONGO_URI`** (default `mongodb://127.0.0.1:27017`). c-lib must be
built with **`USE_MONGOC=1`**.

Requires built libm4engine (same as run_ai_tui). Skips entire suite if library missing.
"""
from __future__ import annotations

import json
import os
import sys
import unittest
import uuid
from unittest.mock import patch

# python_ai on path → `import server.app`
_PYTHON_AI = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _PYTHON_AI not in sys.path:
    sys.path.insert(0, _PYTHON_AI)


def _library_available() -> bool:
    try:
        from engine_ctypes import find_lib

        find_lib()
        return True
    except Exception:
        return False


# c-lib storage.h — chat turns written by engine_append_turn
_MONGO_CHAT_DB = "bot"
_MONGO_CHAT_COLL = "records"


def _find_turn_by_temp_message_id(temp_message_id: str):
    """
    Return one document from bot.records with matching temp_message_id, or None.
    Caller must have pymongo installed and a reachable MongoDB (same URI as the engine).
    """
    from pymongo import MongoClient

    uri = os.environ.get("M4ENGINE_MONGO_URI", "mongodb://127.0.0.1:27017")
    client = MongoClient(uri, serverSelectionTimeoutMS=8000)
    client.admin.command("ping")
    coll = client[_MONGO_CHAT_DB][_MONGO_CHAT_COLL]
    return coll.find_one({"temp_message_id": temp_message_id})


EXPECTED_STATS_KEYS = frozenset(
    {
        "memory_bytes",
        "mongo_connected",
        "mongoc_linked",
        "redis_connected",
        "elk_enabled",
        "elk_connected",
        "ollama_connected",
        "error_count",
        "warning_count",
        "processed",
        "errors",
    }
)


@unittest.skipUnless(_library_available(), "libm4engine not found in python_ai/lib or python_ai/libs")
class TestM4ServerAPI(unittest.TestCase):
    """Integration tests against real ctypes engine (MEMORY or env-configured mode)."""

    srv = None  # set in setUpClass
    client = None

    @classmethod
    def setUpClass(cls):
        # Phase 1 chat uses JWT by default; tests use anonymous tenant/body (legacy).
        os.environ.setdefault("M4_CHAT_REQUIRE_AUTH", "0")
        import server.app as srv

        cls.srv = srv
        if srv._ctx is not None:
            srv._destroy_engine()
        srv._init_engine()
        srv._app.config["TESTING"] = True
        cls.client = srv._app.test_client()

    @classmethod
    def tearDownClass(cls):
        if cls.srv and cls.srv._ctx is not None:
            cls.srv._destroy_engine()

    def test_health_ok(self):
        r = self.client.get("/api/health")
        self.assertEqual(r.status_code, 200)
        data = r.get_json()
        self.assertTrue(data.get("ok"))

    def test_stats_has_expected_keys_and_types(self):
        """Stats JSON shape for FE; zeros/false are normal on a fresh process."""
        r = self.client.get("/api/stats")
        self.assertEqual(r.status_code, 200, r.get_data(as_text=True))
        data = r.get_json()
        self.assertIsInstance(data, dict)
        self.assertEqual(set(data.keys()), EXPECTED_STATS_KEYS)
        self.assertIsInstance(data["memory_bytes"], int)
        self.assertIsInstance(data["mongo_connected"], bool)
        self.assertIsInstance(data["processed"], int)

    def test_history_empty_or_list_on_fresh_engine(self):
        """Fresh start: [] is valid; not an error."""
        r = self.client.get("/api/history")
        self.assertEqual(r.status_code, 200)
        data = r.get_json()
        self.assertIn("messages", data)
        self.assertIn("tenant_id", data)
        self.assertIn("user", data)
        self.assertEqual(data["tenant_id"], data["user"])
        self.assertIsInstance(data["messages"], list)
        self.assertEqual(data["tenant_id"], "default")

    def test_history_tenant_alias_user_maps_to_default(self):
        r = self.client.get("/api/history?tenant_id=user")
        self.assertEqual(r.status_code, 200)
        data = r.get_json()
        self.assertEqual(data["tenant_id"], "default")

    def test_history_reload_query_accepted(self):
        r = self.client.get("/api/history?reload=1")
        self.assertEqual(r.status_code, 200)
        data = r.get_json()
        self.assertIn("messages", data)

    def test_chat_missing_message_400(self):
        r = self.client.post("/api/chat", json={})
        self.assertEqual(r.status_code, 400)
        self.assertIn("error", r.get_json())

    def test_chat_empty_string_400(self):
        r = self.client.post("/api/chat", json={"message": "   "})
        self.assertEqual(r.status_code, 400)

    def test_chat_stream_missing_message_400(self):
        r = self.client.post("/api/chat/stream", json={})
        self.assertEqual(r.status_code, 400)
        self.assertIn("error", r.get_json() or {})

    def test_chat_stream_whitespace_message_400(self):
        r = self.client.post("/api/chat/stream", json={"message": "  \t  "})
        self.assertEqual(r.status_code, 400)

    def test_chat_stream_dispatch_mode_router_when_m4_unset(self):
        """Flask passes ``router`` when ``M4_CHAT_STREAM_MODE`` is unset (no implicit ollama)."""
        captured: list = []

        def stub_dispatch(*, mode, lib, ctx, tid, user_s, message, temp_message_id, sink, gil_held_for_c_callback):
            captured.append(mode)
            sink({"token": "", "temp_message_id": temp_message_id or "", "done": True})

        old = os.environ.pop("M4_CHAT_STREAM_MODE", None)
        try:
            with patch("stream_chat_backends.dispatch_chat_stream", side_effect=stub_dispatch):
                r = self.client.post(
                    "/api/chat/stream",
                    json={"message": "hi", "tenant_id": "default", "user": "default"},
                )
        finally:
            if old is not None:
                os.environ["M4_CHAT_STREAM_MODE"] = old

        self.assertEqual(r.status_code, 200, r.get_data(as_text=True))
        self.assertEqual(captured, ["router"])

    def test_chat_stream_dispatch_mode_ollama_when_m4_set(self):
        """Explicit ``M4_CHAT_STREAM_MODE=ollama`` reaches ``dispatch_chat_stream`` as ``ollama``."""
        captured: list = []

        def stub_dispatch(*, mode, lib, ctx, tid, user_s, message, temp_message_id, sink, gil_held_for_c_callback):
            captured.append(mode)
            sink({"token": "", "temp_message_id": temp_message_id or "", "done": True})

        old = os.environ.get("M4_CHAT_STREAM_MODE")
        try:
            os.environ["M4_CHAT_STREAM_MODE"] = "ollama"
            with patch("stream_chat_backends.dispatch_chat_stream", side_effect=stub_dispatch):
                r = self.client.post(
                    "/api/chat/stream",
                    json={"message": "hi", "tenant_id": "default", "user": "default"},
                )
        finally:
            if old is None:
                os.environ.pop("M4_CHAT_STREAM_MODE", None)
            else:
                os.environ["M4_CHAT_STREAM_MODE"] = old

        self.assertEqual(r.status_code, 200, r.get_data(as_text=True))
        self.assertEqual(captured, ["ollama"])

    def test_chat_stream_sse_finishes_with_done(self):
        """
        Consume the full SSE body: worker always ends with a terminal event (done or error).
        Does not require Ollama if the engine returns an error quickly (e.g. unreachable).
        """
        r = self.client.post(
            "/api/chat/stream",
            json={
                "message": "ping",
                "tenant_id": "default",
                "user": "default",
                "temp_message_id": "00000000-0000-4000-8000-000000000001",
            },
        )
        self.assertEqual(r.status_code, 200)
        text = r.get_data(as_text=True)
        self.assertIn("data:", text)
        events = []
        for block in text.replace("\r\n", "\n").split("\n\n"):
            for line in block.split("\n"):
                if line.startswith("data:"):
                    try:
                        events.append(json.loads(line[5:].strip()))
                    except json.JSONDecodeError:
                        pass
        self.assertTrue(events, "expected at least one SSE data JSON object")
        last = events[-1]
        self.assertIn("done", last)
        self.assertTrue(last["done"] or last.get("token"), msg=last)

    @unittest.skipUnless(
        os.environ.get("M4ENGINE_TEST_SERVER_CHAT", "").strip() in ("1", "true", "yes"),
        "set M4ENGINE_TEST_SERVER_CHAT=1 for live Ollama api_chat",
    )
    def test_chat_live_ollama_when_enabled(self):
        """Optional: requires Ollama; 502 if down (still a valid API contract)."""
        r = self.client.post("/api/chat", json={"message": "Reply with one word: OK"})
        self.assertIn(r.status_code, (200, 502), r.get_data(as_text=True))
        data = r.get_json()
        if r.status_code == 200:
            self.assertIn("reply", data)
            self.assertEqual(data.get("tenant_id"), "default")
            self.assertEqual(data.get("user"), "default")
            self.assertGreater(len(data["reply"]), 0)
        else:
            self.assertIn("error", data)

    @unittest.skipUnless(
        os.environ.get("M4ENGINE_TEST_SERVER_CHAT_STREAM", "").strip() in ("1", "true", "yes"),
        "set M4ENGINE_TEST_SERVER_CHAT_STREAM=1 for live Ollama api_chat_stream",
    )
    def test_chat_stream_live_processed_increments(self):
        """Full SSE read; processed++; if Mongo is connected, assert row in bot.records."""
        r0 = self.client.get("/api/stats")
        self.assertEqual(r0.status_code, 200)
        p0 = r0.get_json()["processed"]

        tmid = str(uuid.uuid4())
        user_msg = "Reply with exactly: STREAM_OK"
        r = self.client.post(
            "/api/chat/stream",
            json={
                "message": user_msg,
                "tenant_id": "default",
                "user": "default",
                "temp_message_id": tmid,
            },
        )
        self.assertEqual(r.status_code, 200, r.get_data(as_text=True))
        r.get_data()  # drain full SSE body

        r1 = self.client.get("/api/stats")
        self.assertEqual(r1.status_code, 200)
        st1 = r1.get_json()
        p1 = st1["processed"]
        self.assertGreaterEqual(
            p1,
            p0 + 1,
            "processed should increase after a completed stream turn",
        )

        if not st1.get("mongo_connected"):
            return

        try:
            doc = _find_turn_by_temp_message_id(tmid)
        except ImportError:
            self.fail(
                "stats.mongo_connected is true but pymongo is not installed. "
                "pip install pymongo   or   pip install -r requirements-dev.txt"
            )
        except Exception as ex:
            self.fail(
                "Mongo verify failed (URI from M4ENGINE_MONGO_URI or default localhost): %s" % ex
            )

        self.assertIsNotNone(
            doc,
            "expected one document in %s.%s with temp_message_id=%r"
            % (_MONGO_CHAT_DB, _MONGO_CHAT_COLL, tmid),
        )
        turn = doc.get("turn") or {}
        self.assertEqual(
            turn.get("input"),
            user_msg,
            "turn.input should match the streamed user message",
        )
        self.assertIn(
            "assistant",
            turn,
            "turn document should contain assistant field (may be empty string)",
        )


if __name__ == "__main__":
    unittest.main()
