"""
Ships ``libm4engine.dylib`` / ``libm4engine.so`` under ``m4engine_binary/native/``.
``engine_ctypes.find_lib()`` uses this when the file is not under ``python_ai/lib`` or ``libs``.
"""
import sys
from pathlib import Path

__version__ = "1.0.0-beta"

_LIB = "libm4engine.dylib" if sys.platform == "darwin" else "libm4engine.so"
_NATIVE = Path(__file__).resolve().parent / "native" / _LIB


def get_library_path():
    """Absolute path to the bundled shared library, or ``None`` if missing."""
    if _NATIVE.is_file():
        return str(_NATIVE)
    return None
