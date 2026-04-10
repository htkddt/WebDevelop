#!/usr/bin/env python3
"""
python_ai — loads ``libm4engine`` via ctypes from ``python_ai/lib/`` or ``python_ai/libs/`` only (git-supplied binary).
Run: python3 run_ai.py "Your question"
"""
import ctypes
import os
import sys

from engine_ctypes import find_lib


def main():
    prompt = " ".join(sys.argv[1:]) if len(sys.argv) > 1 else "Say hello in one sentence."
    lib_path = find_lib()
    lib = ctypes.CDLL(lib_path)

    lib.ollama_query.argtypes = [
        ctypes.c_char_p,
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_size_t,
    ]
    lib.ollama_query.restype = ctypes.c_int

    OL_BUF_SIZE = 32768
    out = ctypes.create_string_buffer(OL_BUF_SIZE)

    host_e = os.environ.get("OLLAMA_HOST", "").strip()
    port_e = os.environ.get("OLLAMA_PORT", "").strip()
    model_e = os.environ.get("OLLAMA_MODEL", "").strip()

    host_p = ctypes.c_char_p(host_e.encode("utf-8")) if host_e else None
    port_i = int(port_e) if port_e else 0
    model_p = ctypes.c_char_p(model_e.encode("utf-8")) if model_e else None

    r = lib.ollama_query(
        host_p,
        port_i,
        model_p,
        prompt.encode("utf-8"),
        out,
        OL_BUF_SIZE,
    )
    if r != 0:
        print("[python_ai] ollama_query failed (is Ollama running?)", file=sys.stderr)
        sys.exit(1)
    print("[python_ai] Reply:", out.value.decode("utf-8", errors="replace").strip() or "(empty)")


if __name__ == "__main__":
    main()
