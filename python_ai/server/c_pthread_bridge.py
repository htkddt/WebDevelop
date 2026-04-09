# -*- coding: utf-8 -*-
"""
Bridge between **libc pthreads** (inside the c-lib) and **CPython’s GIL**.

The c-lib may call Python ctypes callbacks from a **non-Python thread** (e.g.
``api_chat_stream`` worker in ``api.c``, geo_learning worker in ``geo_learning.c``).
Those callbacks **must** wrap Python work in ``PyGILState_Ensure`` /
``PyGILState_Release`` or the interpreter can deadlock or corrupt state.

This module provides a small **context manager** so handlers stay correct without
duplicating ctypes boilerplate. It does **not** change or replace pthread usage in C.

**Serialization of ``api_*`` on ``ctx``:** Flask uses ``threading.Lock()`` in ``app.py``
(``_ctx_lock``) so **Python-initiated** c-lib calls on the same ``ctx`` do not race
each other. Internal C workers still follow the library’s own mutex rules; we do not
modify those in C from here.
"""
from __future__ import annotations

import ctypes
from contextlib import contextmanager
from typing import Generator

_PyGILState_Ensure = ctypes.pythonapi.PyGILState_Ensure
_PyGILState_Ensure.restype = ctypes.c_void_p
_PyGILState_Release = ctypes.pythonapi.PyGILState_Release
_PyGILState_Release.argtypes = [ctypes.c_void_p]


@contextmanager
def gil_held_for_c_callback() -> Generator[None, None, None]:
    """
    Use inside a ctypes callback that may run on a **pthread** created by the c-lib::

        @CFUNCTYPE(...)
        def on_token(...):
            with gil_held_for_c_callback():
                ...
    """
    gstate = _PyGILState_Ensure()
    try:
        yield
    finally:
        _PyGILState_Release(gstate)
