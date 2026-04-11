"""
M4 AI Engine — Python bindings.

Usage:
    from m4engine import Engine

    engine = Engine({"mode": 2, "debug_modules": ["ai_agent"]})
    reply = engine.chat("default", "user_1", "Hello!")
    print(reply)
    engine.close()

Or as context manager:
    with Engine({"mode": 0}) as e:
        print(e.chat("default", "user_1", "Hello!"))
"""

import ctypes
import json
import os
import platform

def _find_lib():
    """Find libm4engine in order: env var → package native → common paths."""
    env = os.environ.get("M4ENGINE_LIB")
    if env and os.path.isfile(env):
        return env

    # Bundled with pip package
    pkg_dir = os.path.join(os.path.dirname(__file__), "_native", "lib")
    ext = "dylib" if platform.system() == "Darwin" else "so"
    bundled = os.path.join(pkg_dir, f"libm4engine.{ext}")
    if os.path.isfile(bundled):
        return bundled

    # Common relative paths
    for rel in ["lib", "../lib", "../c-lib/lib", "../../c-lib/lib"]:
        p = os.path.join(os.path.dirname(__file__), rel, f"libm4engine.{ext}")
        if os.path.isfile(p):
            return p

    raise FileNotFoundError(
        "libm4engine not found. Set M4ENGINE_LIB env var or install with: "
        "pip install -e bindings/python (from c-lib repo)"
    )

_lib = ctypes.CDLL(_find_lib())
_lib.api_create.restype = ctypes.c_void_p
_lib.api_create.argtypes = [ctypes.c_char_p]
_lib.api_destroy.argtypes = [ctypes.c_void_p]
_lib.api_chat.argtypes = [
    ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
    ctypes.c_char_p, ctypes.c_size_t,
    ctypes.c_void_p, ctypes.c_void_p,
]
_lib.api_chat.restype = ctypes.c_int
_lib.api_load_chat_history.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
_lib.api_load_chat_history.restype = ctypes.c_int

StreamCB = ctypes.CFUNCTYPE(
    None, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_void_p
)


class Engine:
    """M4 AI Engine context.

    Args:
        opts: dict or JSON string. See docs/api.md for all options.
              Pass {} or None for defaults.
    """

    def __init__(self, opts=None):
        if opts is None:
            json_str = b"{}"
        elif isinstance(opts, dict):
            json_str = json.dumps(opts).encode()
        elif isinstance(opts, str):
            json_str = opts.encode()
        elif isinstance(opts, bytes):
            json_str = opts
        else:
            raise TypeError("opts must be dict, str, bytes, or None")

        self._ctx = _lib.api_create(json_str)
        if not self._ctx:
            raise RuntimeError("api_create failed — check options and stderr logs")

    def close(self):
        if self._ctx:
            _lib.api_destroy(self._ctx)
            self._ctx = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def __del__(self):
        self.close()

    def chat(self, tenant_id="default", user_id="default", message="", buf_size=32768):
        """Sync chat. Returns assistant reply string or None on error."""
        reply = ctypes.create_string_buffer(buf_size)
        rc = _lib.api_chat(
            self._ctx,
            tenant_id.encode() if isinstance(tenant_id, str) else tenant_id,
            user_id.encode() if isinstance(user_id, str) else user_id,
            message.encode() if isinstance(message, str) else message,
            reply, buf_size, None, None,
        )
        return reply.value.decode("utf-8") if rc == 0 else None

    def chat_stream(self, tenant_id="default", user_id="default", message="", on_token=None):
        """Stream chat. Calls on_token(text, done) for each token. Returns full reply."""
        tokens = []

        @StreamCB
        def _cb(token, msg_id, done, ud):
            text = token.decode("utf-8") if token else ""
            if not done and text:
                tokens.append(text)
            if on_token:
                on_token(text, bool(done))

        reply = ctypes.create_string_buffer(32768)
        _lib.api_chat(
            self._ctx,
            tenant_id.encode(), user_id.encode(), message.encode(),
            reply, 32768, _cb, None,
        )
        return "".join(tokens)

    def load_history(self, tenant_id="default", user_id="default"):
        """Load chat history from MongoDB."""
        return _lib.api_load_chat_history(
            self._ctx, tenant_id.encode(), user_id.encode()
        )
