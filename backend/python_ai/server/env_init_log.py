# -*- coding: utf-8 -*-
"""
Engine-init diagnostics: dump ``server/.env`` as parsed key/value (file only) with redaction.

``load_server_env`` already merged this file into ``os.environ`` (non-overriding). This log shows
exactly what is **declared in the file** so you can compare with ``log_resolved_engine_options_at_startup``
(process env + defaults + DB merge).

Disable: ``M4_LOG_DOTENV_FILE_AT_INIT=0|false|no|off``.
"""
from __future__ import annotations

import os
import sys
from typing import Dict, Optional

_LOG_ENV = "M4_LOG_DOTENV_FILE_AT_INIT"


def _redact_dotenv_value(key: str, value: str) -> str:
    ku = key.upper()
    if "URI" in ku or "PASSWORD" in ku or "SECRET" in ku:
        return f"<set, {len(value)} chars>"
    if "TOKEN" in ku and "M4_DEBUG_CHAT" not in ku:
        return f"<set, {len(value)} chars>"
    if ku.endswith("_KEY") or ku.endswith("_SECRET"):
        return f"<set, {len(value)} chars>"
    if len(value) > 160:
        return repr(value[:157] + "...")
    return repr(value)


def _parse_dotenv_file(path: str) -> Dict[str, Optional[str]]:
    """Minimal parser if python-dotenv is absent."""
    out: Dict[str, Optional[str]] = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            if s.lower().startswith("export "):
                s = s[7:].strip()
            if "=" not in s:
                continue
            key, _, rest = s.partition("=")
            key = key.strip()
            if not key:
                continue
            val = rest.strip()
            if (val.startswith('"') and val.endswith('"')) or (val.startswith("'") and val.endswith("'")):
                val = val[1:-1]
            out[key] = val
    return out


def log_dotenv_file_at_engine_init(server_dir: str) -> None:
    flag = os.environ.get(_LOG_ENV, "1").strip().lower()
    if flag in ("0", "false", "no", "off"):
        return
    path = os.path.join(server_dir, ".env")
    print(
        "[M4] engine init — server/.env snapshot (file contents only; shell exports not listed here):",
        file=sys.stderr,
    )
    if not os.path.isfile(path):
        print(f"[M4]   <no file at {path}>", file=sys.stderr)
        print(
            "[M4]   (variables may still come from the shell or system environment; see next block for merged options.)",
            file=sys.stderr,
        )
        return

    raw: Dict[str, Optional[str]]
    try:
        from dotenv import dotenv_values  # noqa: PLC0415

        raw = {k: v for k, v in dotenv_values(path).items() if k is not None}
    except ImportError:
        raw = _parse_dotenv_file(path)

    if not raw:
        print(f"[M4]   <file empty or no KEY= lines: {path}>", file=sys.stderr)
        return

    for k in sorted(raw.keys(), key=lambda x: (x or "").lower()):
        v = raw.get(k)
        if v is None or str(v).strip() == "":
            print(f"[M4]   .env {k}=<empty>", file=sys.stderr)
        else:
            vs = str(v)
            print(f"[M4]   .env {k}={_redact_dotenv_value(k, vs)}", file=sys.stderr)
