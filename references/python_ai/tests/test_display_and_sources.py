"""
Tests for display formatting (epoch ts, You/Bot prefix), SOURCE_LABELS, and optional c-lib API.
Run: python3 -m unittest python_ai.tests.test_display_and_sources -v
  or from python_ai/: python3 -m unittest tests.test_display_and_sources -v
"""
import time
import unittest

# Display helpers (no curses)
from display_utils import format_ts, build_message_prefix

# Engine constants (no lib load)
from engine_ctypes import (
    SOURCE_LABELS,
    API_DEFAULT_TENANT_ID,
    API_SOURCE_CLOUD,
    API_SOURCE_MEMORY,
    API_SOURCE_REDIS,
    API_SOURCE_MONGODB,
    API_SOURCE_OLLAMA,
    completion_source_label,
    M4ENGINE_MODE_ONLY_MEMORY,
    M4ENGINE_MODE_MONGO_REDIS_ELK,
)


# --- Source constants (same as c-lib API_SOURCE_*) ---
class TestSourceConstants(unittest.TestCase):
    def test_source_labels_keys(self):
        self.assertEqual(SOURCE_LABELS[API_SOURCE_MEMORY], "MEMORY")
        self.assertEqual(SOURCE_LABELS[API_SOURCE_REDIS], "REDIS")
        self.assertEqual(SOURCE_LABELS[API_SOURCE_MONGODB], "MONGODB")
        self.assertEqual(SOURCE_LABELS[API_SOURCE_OLLAMA], "OLLAMA")
        self.assertEqual(SOURCE_LABELS[API_SOURCE_CLOUD], "CLOUD")

    def test_source_chars(self):
        self.assertEqual(API_SOURCE_MEMORY, ord("M"))
        self.assertEqual(API_SOURCE_REDIS, ord("R"))
        self.assertEqual(API_SOURCE_MONGODB, ord("G"))
        self.assertEqual(API_SOURCE_OLLAMA, ord("O"))
        self.assertEqual(API_SOURCE_CLOUD, ord("C"))

    def test_completion_source_from_llm_prefix(self):
        self.assertEqual(completion_source_label(ord("C"), "groq:llama-3.1-8b-instant"), "GROQ")
        self.assertEqual(completion_source_label(ord("C"), "cerebras:llama3.1-8b"), "CEREBRAS")
        self.assertEqual(completion_source_label(ord("C"), "gemini:gemini-2.0-flash"), "GEMINI")
        self.assertEqual(completion_source_label(ord("O"), "ollama:qwen2.5"), "OLLAMA")
        self.assertEqual(completion_source_label(ord("R"), "redis_rag"), "REDIS_RAG")

    def test_completion_source_fallback_coarse(self):
        self.assertEqual(completion_source_label(ord("O"), None), "OLLAMA")
        self.assertEqual(completion_source_label(ord("C"), ""), "CLOUD")
        self.assertEqual(completion_source_label(ord("M"), None), "MEMORY")
        self.assertEqual(completion_source_label(None, None), "MONGODB")

    def test_completion_source_mongo_row(self):
        self.assertEqual(completion_source_label(None, "groq:x"), "GROQ")

    def test_default_tenant(self):
        self.assertEqual(API_DEFAULT_TENANT_ID, b"default")

    def test_mode_constants(self):
        self.assertEqual(M4ENGINE_MODE_ONLY_MEMORY, 0)
        self.assertEqual(M4ENGINE_MODE_MONGO_REDIS_ELK, 3)


# --- format_ts: epoch ms → HH:MM:SS, legacy passthrough ---
class TestFormatTs(unittest.TestCase):
    def test_empty_or_none(self):
        self.assertEqual(format_ts(None), "")
        self.assertEqual(format_ts(""), "")
        self.assertEqual(format_ts("   "), "")

    def test_epoch_ms_string(self):
        ts = format_ts("1731900000000")
        self.assertIsInstance(ts, str)
        self.assertEqual(len(ts), 8)  # HH:MM:SS
        self.assertEqual(ts[2], ":")
        self.assertEqual(ts[5], ":")
        self.assertTrue(all(c in "0123456789:" for c in ts))

    def test_epoch_seconds_string(self):
        ts = format_ts("1731900000")
        self.assertIsInstance(ts, str)
        self.assertEqual(len(ts), 8)
        self.assertIn(":", ts)

    def test_legacy_passthrough(self):
        self.assertEqual(format_ts("09:09:47"), "09:09:47")
        self.assertEqual(format_ts("12:00:00"), "12:00:00")


# --- build_message_prefix: You [ts]: / Bot [ts]-[label]: ---
class TestBuildMessagePrefix(unittest.TestCase):
    def test_user_with_ts(self):
        p = build_message_prefix("user", "1731900000000", "MEMORY")
        self.assertTrue(p.startswith("You ["))
        self.assertIn("]: ", p)

    def test_user_no_ts(self):
        p = build_message_prefix("user", None, "MEMORY")
        self.assertEqual(p, "You: ")

    def test_assistant_with_ts(self):
        p = build_message_prefix("assistant", "1731900000000", "OLLAMA")
        self.assertTrue(p.startswith("Bot ["))
        self.assertIn("]-[OLLAMA]: ", p)

    def test_assistant_no_ts(self):
        p = build_message_prefix("assistant", None, "REDIS")
        self.assertEqual(p, "Bot-[REDIS]: ")


# --- History tuple shape and source label ---
class TestHistoryDisplayShape(unittest.TestCase):
    def test_user_item_prefix(self):
        item = ("user", "hello", str(int(time.time() * 1000)), "MEMORY")
        prefix = build_message_prefix(item[0], item[2], item[3])
        self.assertTrue(prefix.startswith("You "))
        self.assertNotIn("hello", prefix)

    def test_assistant_item_prefix_ollama(self):
        item = ("assistant", "Đã lâu rồi!", "1731900000000", "OLLAMA")
        prefix = build_message_prefix(item[0], item[2], item[3])
        self.assertIn("Bot ", prefix)
        self.assertIn("OLLAMA", prefix)

    def test_bot_role_normalized(self):
        item = ("bot", "reply", None, "MONGODB")
        prefix = build_message_prefix(item[0], item[2], item[3])
        self.assertIn("Bot-", prefix)
        self.assertIn("MONGODB", prefix)


# --- Helpers: JSON api_create ---
def _with_api_context(lib, json_config, run):
    """Create context with JSON, call run(ctx), destroy. Returns run's return value."""
    import json as _json
    config_b = _json.dumps(json_config).encode("utf-8") if isinstance(json_config, dict) else json_config
    ctx = lib.api_create(config_b)
    if ctx is None:
        return None
    try:
        return run(ctx)
    finally:
        lib.api_destroy(ctx)


# --- Optional: c-lib API (skip if lib not found) ---
class TestApiWithLib(unittest.TestCase):
    """Requires libm4engine in python_ai/lib or libs."""

    @classmethod
    def setUpClass(cls):
        try:
            from engine_ctypes import load_lib
            cls._lib = load_lib()
        except (FileNotFoundError, OSError):
            cls._lib = None

    def setUp(self):
        if self._lib is None:
            self.skipTest("libm4engine not found (place in python_ai/lib or python_ai/libs)")

    def test_get_history_message_out_of_range_returns_failure(self):
        import ctypes
        def get_msg_0(ctx):
            role_buf = ctypes.create_string_buffer(32)
            content_buf = ctypes.create_string_buffer(2048)
            source_char = ctypes.c_char()
            ts_buf = ctypes.create_string_buffer(64)
            return self._lib.api_get_history_message(
                ctx, 0, role_buf, 32, content_buf, 2048,
                ctypes.byref(source_char), ts_buf, 64,
                None, 0,
            )
        ret = _with_api_context(self._lib, {"mode": 0}, get_msg_0)
        self.assertEqual(ret, -1)

    def test_fresh_context_stats(self):
        import ctypes
        from engine_ctypes import ApiStats
        def check_stats(ctx):
            stats = ApiStats()
            self._lib.api_get_stats(ctx, ctypes.byref(stats))
            return stats.last_reply_source == b"\x00" or stats.last_reply_source == 0
        ok = _with_api_context(self._lib, {"mode": 0}, check_stats)
        self.assertTrue(ok)


if __name__ == "__main__":
    unittest.main()
