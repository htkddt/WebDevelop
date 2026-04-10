# -*- coding: utf-8 -*-
"""
Unit tests for stream mode resolution.

Run from python_ai/:
  python3 -m unittest tests.test_chat_stream_config -v
"""
from __future__ import annotations

import os
import sys
import unittest

_PYTHON_AI = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _PYTHON_AI not in sys.path:
    sys.path.insert(0, _PYTHON_AI)


class TestChatStreamConfig(unittest.TestCase):
    def test_resolved_chat_stream_mode_normalizes(self):
        from server.stream_chat_backends import resolved_chat_stream_mode

        self.assertEqual(resolved_chat_stream_mode(None), "default")
        self.assertEqual(resolved_chat_stream_mode(""), "default")
        self.assertEqual(resolved_chat_stream_mode("   "), "default")
        self.assertEqual(resolved_chat_stream_mode("ollama"), "ollama")
        self.assertEqual(resolved_chat_stream_mode("OLLAMA"), "ollama")
        self.assertEqual(resolved_chat_stream_mode("router"), "router")


if __name__ == "__main__":
    unittest.main()
