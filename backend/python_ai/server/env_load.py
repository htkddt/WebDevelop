# -*- coding: utf-8 -*-
"""Load ``.env`` then ``.env.test`` from the server directory — no shell ``export`` needed.

Order: ``.env`` first (does not override variables already set in the process environment),
then ``.env.test`` with ``override=True`` so test/local overrides apply on top.

Missing files are ignored. If ``python-dotenv`` is not installed, this is a no-op.

Engine init also logs a redacted **file-only** copy of ``.env`` (``env_init_log.log_dotenv_file_at_engine_init``)
and merged **full options** (``training.full_options.log_resolved_engine_options_at_startup``); disable with
``M4_LOG_DOTENV_FILE_AT_INIT=0`` / ``M4_LOG_ENGINE_RESOLVED_OPTIONS=0``.
"""
from __future__ import annotations

import os


def load_server_env(server_dir: str) -> None:
    try:
        from dotenv import load_dotenv
    except ImportError:
        return
    base = os.path.join(server_dir, ".env")
    test = os.path.join(server_dir, ".env.test")
    if os.path.isfile(base):
        load_dotenv(base, override=False)
    if os.path.isfile(test):
        load_dotenv(test, override=True)
