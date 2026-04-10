#!/usr/bin/env python3
"""
Upsert ``bot_c_lib_settings`` with every ``OPTION_KEYS`` field generated from current env.

Usage (from repo):
  cd python_ai
  python3 scripts/seed_bot_c_lib_full.py
  python3 scripts/seed_bot_c_lib_full.py --use-full-env   # base = get_full_options() (like M4ENGINE_SERVER_MAX=0)

Loads ``server/.env`` via ``env_load`` when present. Requires app Mongo unless you only use in-memory
identity (then values land in process memory — start the server in the same shell to test merge).
"""
from __future__ import annotations

import argparse
import json
import os
import sys

_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
_PYTHON_AI_ROOT = os.path.dirname(_SCRIPT_DIR)
_SERVER_DIR = os.path.join(_PYTHON_AI_ROOT, "server")

if _PYTHON_AI_ROOT not in sys.path:
    sys.path.insert(0, _PYTHON_AI_ROOT)
if _SERVER_DIR not in sys.path:
    sys.path.insert(0, _SERVER_DIR)

from env_load import load_server_env  # noqa: E402

load_server_env(_SERVER_DIR)


def main() -> int:
    p = argparse.ArgumentParser(description="Upsert full bot_c_lib_settings from env snapshot.")
    p.add_argument(
        "--use-full-env",
        action="store_true",
        help="Use get_full_options() as base instead of get_max_options() (ignore M4ENGINE_SERVER_MAX).",
    )
    p.add_argument(
        "--dry-run",
        action="store_true",
        help="Print JSON only; do not write Mongo / memory store.",
    )
    args = p.parse_args()

    from training.full_options import build_snapshot_bot_c_lib_values_for_db  # noqa: E402

    use_max = not args.use_full_env
    snap = build_snapshot_bot_c_lib_values_for_db(use_max=use_max)
    print(json.dumps(snap, indent=2, sort_keys=True, default=str))
    if args.dry_run:
        return 0

    from bot_c_lib_settings_store import upsert_bot_c_lib_full_options_snapshot  # noqa: E402

    out = upsert_bot_c_lib_full_options_snapshot(
        use_max=use_max,
        replace=True,
        updated_by="script:seed_bot_c_lib_full.py",
    )
    print(f"\n# upserted {len(out)} keys", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
