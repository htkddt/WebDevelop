# -*- coding: utf-8 -*-
"""
Ollama compile-time defaults are defined only in ``c-lib/include/ollama.h``.

Python reads the same values from the built shared library via ``api_build_ollama_*``
(see ``engine_ctypes``). Do not add string literals here — rebuild c-lib after changing
``OLLAMA_DEFAULT_*`` in ``ollama.h``. See ``c-lib/.cursor/default_models.md``.
"""
from __future__ import annotations

__all__ = ["OLLAMA_DEFAULT_MODEL", "OLLAMA_DEFAULT_EMBED_MODEL"]


def __getattr__(name: str):
    if name == "OLLAMA_DEFAULT_MODEL":
        from engine_ctypes import c_build_ollama_default_chat_model

        return c_build_ollama_default_chat_model()
    if name == "OLLAMA_DEFAULT_EMBED_MODEL":
        from engine_ctypes import c_build_ollama_default_embed_model

        return c_build_ollama_default_embed_model()
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
