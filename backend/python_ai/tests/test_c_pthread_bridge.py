# -*- coding: utf-8 -*-
"""Tests for ``server/c_pthread_bridge.py`` (GIL + c-lib pthread callbacks)."""
from __future__ import annotations

import os
import sys
import threading
import unittest

_PYTHON_AI = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _PYTHON_AI not in sys.path:
    sys.path.insert(0, _PYTHON_AI)
_SERVER_DIR = os.path.join(_PYTHON_AI, "server")
if _SERVER_DIR not in sys.path:
    sys.path.insert(0, _SERVER_DIR)

from c_pthread_bridge import gil_held_for_c_callback  # noqa: E402


class TestGilBridge(unittest.TestCase):
    def test_nested_gil_context_is_safe(self) -> None:
        """PyGILState can nest; context manager must release in LIFO order."""
        with gil_held_for_c_callback():
            x = 1
            with gil_held_for_c_callback():
                x += 1
            self.assertEqual(x, 2)

    def test_callback_from_pthread_style_thread(self) -> None:
        """Simulate c-lib invoking a ctypes callback from another OS thread."""
        out: list[int] = []

        def fake_c_callback() -> None:
            with gil_held_for_c_callback():
                out.append(threading.get_ident())

        th = threading.Thread(target=fake_c_callback)
        th.start()
        th.join(timeout=5.0)
        self.assertEqual(len(out), 1)


if __name__ == "__main__":
    unittest.main()
