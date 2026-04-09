"""
ctypes bindings for c-lib public API (api.h).
Use from run_ai_tui.py, server/app.py, or other Python consumers.

**Engine binary:** (1) under ``python_ai/lib/`` or ``python_ai/libs/``, or (2) install the pip
package **m4engine-binary** (e.g. ``pip install "m4engine-binary @ git+https://...#subdirectory=..."``)
which ships ``native/libm4engine.{dylib,so}``. ``find_lib()`` checks local ``lib/`` / ``libs/`` first,
then the installed package. Python does **not** resolve a monorepo ``c-lib/`` checkout.

Option / chat capacity logging is emitted inside c-lib on ``api_create`` / ``api_chat``
(stderr). Toggle with ``M4_LOG_API_CREATE_OPTS`` and ``M4_LOG_API_CHAT`` (unset = on;
set to ``0`` / ``false`` / ``no`` / ``off`` / empty to disable).
"""
import ctypes
import os
import sys
from typing import Optional

LIB_NAME = "libm4engine.dylib" if sys.platform == "darwin" else "libm4engine.so"

def find_lib():
    """Resolve ``libm4engine``: ``python_ai/lib`` / ``libs``, then pip package ``m4engine_binary``."""
    this_dir = os.path.dirname(os.path.abspath(__file__))
    for candidate in (
        os.path.join(this_dir, "lib", LIB_NAME),
        os.path.join(this_dir, "libs", LIB_NAME),
    ):
        if os.path.isfile(candidate):
            return candidate
    try:
        import m4engine_binary  # type: ignore

        pip_path = m4engine_binary.get_library_path()
        if pip_path and os.path.isfile(pip_path):
            return pip_path
    except ImportError:
        pass
    raise FileNotFoundError(
        f"Library not found: {LIB_NAME}. Either copy it to python_ai/lib/ or python_ai/libs/, or "
        f'pip install the engine wheel/git package, e.g. pip install "m4engine-binary @ git+https://...".'
    )

# C types
c_size_t = ctypes.c_size_t
c_uint64 = ctypes.c_uint64

# Smart topic (mini AI switch) — intent-based temperature. See c-lib/docs/smart_topic.md.
MINI_AI_TYPE_TINY = 0
MINI_AI_TYPE_B2 = 1
MINI_AI_TYPE_SMALL = 2


class SmartTopicOptions(ctypes.Structure):
    """smart_topic_options_t: enable, library_type, execution_mode, mini_ai_collection, model_tiny, model_b2."""
    _fields_ = [
        ("enable", ctypes.c_int),              # 1 = on, 0 = off
        ("library_type", ctypes.c_int),       # MINI_AI_TYPE_TINY etc.
        ("execution_mode", ctypes.c_int),      # M4ENGINE_MODE_*
        ("mini_ai_collection", ctypes.c_char_p),  # NULL => "smart_topic"
        ("model_tiny", ctypes.c_char_p),      # e.g. "tinyllama"
        ("model_b2", ctypes.c_char_p),
    ]


class ModelSwitchLaneEntry(ctypes.Structure):
    """model_switch_lane_entry_t — must match include/model_switch.h layout."""
    _fields_ = [
        ("key", ctypes.c_char_p),
        ("model", ctypes.c_char_p),
        ("inject", ctypes.c_char_p),
    ]


class ModelSwitchOptions(ctypes.Structure):
    """model_switch_options_t — NULL from Python = C default / getenv only."""
    _fields_ = [
        ("lanes", ctypes.POINTER(ModelSwitchLaneEntry)),
        ("lane_count", c_size_t),
        ("fallback_model", ctypes.c_char_p),
        ("flags", ctypes.c_uint32),
        ("adaptive_profile_id", ctypes.c_char_p),
    ]


class ApiOptions(ctypes.Structure):
    """Must match api_options_t in include/api.h field-for-field (order and types)."""
    _fields_ = [
        ("mode", ctypes.c_int),
        ("mongo_uri", ctypes.c_char_p),
        ("redis_host", ctypes.c_char_p),
        ("redis_port", ctypes.c_int),
        ("es_host", ctypes.c_char_p),
        ("es_port", ctypes.c_int),
        ("log_db", ctypes.c_char_p),
        ("log_coll", ctypes.c_char_p),
        ("context_batch_size", ctypes.c_int),  # 0 = default (30); limits history cycles in context
        ("smart_topic_opts", ctypes.POINTER(SmartTopicOptions)),  # NULL = smart topic off
        ("inject_geo_knowledge", ctypes.c_int),
        ("disable_auto_system_time", ctypes.c_int),
        ("geo_authority", ctypes.c_int),
        ("model_switch_opts", ctypes.POINTER(ModelSwitchOptions)),  # NULL = env / OLLAMA_MODEL only
        ("vector_gen_backend", ctypes.c_int),  # 0 = custom hash embed, 1 = Ollama /api/embed
        ("vector_ollama_model", ctypes.c_char_p),  # when backend==1; NULL = default embed resolution
        ("embed_migration_autostart", ctypes.c_int),
        ("session_idle_seconds", ctypes.c_int),  # 0 = use M4_SESSION_IDLE_SECONDS / default
        ("shared_collection_mongo_uri", ctypes.c_char_p),
        ("shared_collection_json_path", ctypes.c_char_p),
        ("shared_collection_backfill_db", ctypes.c_char_p),
        ("learning_terms_path", ctypes.c_char_p),
        ("enable_learning_terms", ctypes.c_int),
        ("defer_learning_terms_load", ctypes.c_int),
        # --- new in v1.1 (api.h update) ---
        ("default_persona", ctypes.c_char_p),         # initial API_PROMPT_TAG_PERSONA; NULL = compiled-in default
        ("default_instructions", ctypes.c_char_p),     # initial API_PROMPT_TAG_INSTRUCTIONS; NULL = none
        ("default_model_lane", ctypes.c_int),           # M4_API_MODEL_LANE_*; 0 = DEFAULT
        ("geo_authority_csv_path", ctypes.c_char_p),   # path to geo authority CSV; NULL = disabled
        ("geo_migrate_legacy", ctypes.c_int),           # non-zero: run geo_atlas legacy migration at init
        ("debug_modules", ctypes.POINTER(ctypes.c_char_p)),  # array of module key strings; NULL = no debug
        ("debug_module_count", ctypes.c_int),
    ]

class ApiStats(ctypes.Structure):
    _fields_ = [
        ("memory_bytes", c_uint64),
        ("mongo_connected", ctypes.c_int),
        ("redis_connected", ctypes.c_int),
        ("elk_enabled", ctypes.c_int),
        ("elk_connected", ctypes.c_int),
        ("ollama_connected", ctypes.c_int),  # 1 = Ollama running (health check), 0 = not
        ("error_count", c_uint64),
        ("warning_count", c_uint64),
        ("processed", c_uint64),
        ("errors", c_uint64),
        ("mongoc_linked", ctypes.c_int),  # 1 = c-lib built with USE_MONGOC (else mongo_connected stays 0)
        # --- new in v1.1 ---
        ("last_reply_source", ctypes.c_char),  # API_SOURCE_* char; 0 if no reply yet
        ("last_chat_wire", ctypes.c_uint),      # API_CHAT_WIRE_*; 0 if none
        ("last_llm_model", ctypes.c_char * 160),  # e.g. "groq:llama-3.1-8b-instant"
    ]

M4ENGINE_MODE_ONLY_MEMORY = 0
M4ENGINE_MODE_ONLY_MONGO = 1
M4ENGINE_MODE_MONGO_REDIS = 2
M4ENGINE_MODE_MONGO_REDIS_ELK = 3

API_VECTOR_GEN_CUSTOM = 0
API_VECTOR_GEN_OLLAMA = 1

API_CONTEXT_BATCH_SIZE_DEFAULT = 30
OL_BUF_SIZE = 32768

# Tenant key: use the same for api_load_chat_history and api_chat (see api.h API_DEFAULT_TENANT_ID).
API_DEFAULT_TENANT_ID = b"default"
# api.h API_PROMPT_TAG_INSTRUCTIONS — session-only prompt block (not persisted in turn.input).
API_PROMPT_TAG_INSTRUCTIONS = b"instructions"

# Message source (api.h API_SOURCE_*): for display "Bot [time]-[type]:"
API_SOURCE_MEMORY = ord("M")
API_SOURCE_REDIS = ord("R")
API_SOURCE_MONGODB = ord("G")
API_SOURCE_OLLAMA = ord("O")
API_SOURCE_CLOUD = ord("C")

# Must match api.h API_LLM_ROUTE_PREFIX_* (metadata.llm_model_id / cloud_llm labels).
API_LLM_ROUTE_PREFIX_GROQ = "groq"
API_LLM_ROUTE_PREFIX_CEREBRAS = "cerebras"
API_LLM_ROUTE_PREFIX_GEMINI = "gemini"
API_LLM_ROUTE_PREFIX_OLLAMA = "ollama"
# api.h API_LLM_MODEL_ID_REDIS_RAG — Redis vector cache hit (session / Mongo llm_model_id).
API_LLM_MODEL_ID_REDIS_RAG = "redis_rag"

# api.h API_CHAT_BACKEND_ENV_* — ``M4_CHAT_BACKEND`` values (see cloud_llm.c).
API_CHAT_BACKEND_ENV_OLLAMA = "ollama"
API_CHAT_BACKEND_ENV_CLOUD = "cloud"

SOURCE_LABELS = {
    API_SOURCE_MEMORY: "MEMORY",
    API_SOURCE_REDIS: "REDIS",
    API_SOURCE_MONGODB: "MONGODB",
    API_SOURCE_OLLAMA: "OLLAMA",
    API_SOURCE_CLOUD: "CLOUD",
}


def _norm_source_byte(source_byte) -> Optional[int]:
    """Normalize api_get_history_message / api_get_last_reply_source char to 0–255, or None if unset."""
    if source_byte is None:
        return None
    if isinstance(source_byte, int):
        return (source_byte & 0xFF) if source_byte != 0 else None
    if isinstance(source_byte, bytes) and len(source_byte) == 1:
        v = source_byte[0]
        return v if v != 0 else None
    return None


def completion_source_label(source_byte, llm_model: Optional[str] = None) -> str:
    """
    One stable label for where an assistant completion came from (API + history).

    Prefer ``llm_model`` / ``metadata.llm_model_id`` shape from c-lib (``groq:…``, ``cerebras:…``,
    ``gemini:…``, ``ollama:…``, ``redis_rag``, ``external``). Otherwise fall back to coarse
    ``API_SOURCE_*`` via ``SOURCE_LABELS`` (``CLOUD`` if cloud without a known prefix).
    """
    lm = (llm_model or "").strip()
    if lm:
        low = lm.lower()
        if low.startswith(f"{API_LLM_ROUTE_PREFIX_GROQ}:"):
            return "GROQ"
        if low.startswith(f"{API_LLM_ROUTE_PREFIX_CEREBRAS}:"):
            return "CEREBRAS"
        if low.startswith(f"{API_LLM_ROUTE_PREFIX_GEMINI}:"):
            return "GEMINI"
        if low.startswith(f"{API_LLM_ROUTE_PREFIX_OLLAMA}:"):
            return "OLLAMA"
        if low == API_LLM_MODEL_ID_REDIS_RAG:
            return "REDIS_RAG"
        if low == "external":
            return "EXTERNAL"
    o = _norm_source_byte(source_byte)
    if o is None:
        return "MONGODB"
    return SOURCE_LABELS.get(o, "MONGODB")

# Last assistant reply wire/API family (api.h API_CHAT_WIRE_*); from call site, not model_id string.
API_CHAT_WIRE_NONE = 0
API_CHAT_WIRE_OPENAI_CHAT = 1
API_CHAT_WIRE_GEMINI = 2
API_CHAT_WIRE_OLLAMA = 3
API_CHAT_WIRE_REDIS_RAG = 4
API_CHAT_WIRE_EXTERNAL = 5
CHAT_WIRE_LABELS = {
    API_CHAT_WIRE_NONE: "NONE",
    API_CHAT_WIRE_OPENAI_CHAT: "OPENAI_CHAT",
    API_CHAT_WIRE_GEMINI: "GEMINI",
    API_CHAT_WIRE_OLLAMA: "OLLAMA",
    API_CHAT_WIRE_REDIS_RAG: "REDIS_RAG",
    API_CHAT_WIRE_EXTERNAL: "EXTERNAL",
}

# api_stream_token_cb — invoked from c-lib pthread; use c_pthread_bridge.gil_held_for_c_callback in handlers.
# Signature: (token, msg_id, done_flag, userdata)
STREAM_TOKEN_CB = ctypes.CFUNCTYPE(
    None,
    ctypes.c_char_p,   # token UTF-8 fragment
    ctypes.c_char_p,   # msg_id (stable for this stream)
    ctypes.c_int,       # done_flag: 1 = stream finished
    ctypes.c_void_p,   # userdata
)

M4_API_MODEL_LANE_DEFAULT = 0
M4_API_MODEL_LANE_EDUCATION = 1
M4_API_MODEL_LANE_BUSINESS = 2
M4_API_MODEL_LANE_TECH = 3
M4_API_MODEL_LANE_CHAT = 4


_loaded_lib = None


def load_lib():
    """Load libm4engine and bind the public API surface (7 functions + build-time helpers).

    Matches ``include/api.h`` + ``include/api_build.h`` from c-lib.
    See docs/api.md for the full contract.
    """
    global _loaded_lib
    if _loaded_lib is not None:
        return _loaded_lib
    lib = ctypes.CDLL(find_lib())

    # ── 1a. api_create(const char *json_opts) -> api_context_t* (recommended) ─
    lib.api_create.argtypes = [ctypes.c_char_p]
    lib.api_create.restype = ctypes.c_void_p

    # ── 1b. api_create_with_opts(const api_options_t *opts) (legacy C struct) ─
    lib.api_create_with_opts.argtypes = [ctypes.POINTER(ApiOptions)]
    lib.api_create_with_opts.restype = ctypes.c_void_p

    # ── 2. api_destroy(ctx) ──────────────────────────────────────────────────
    lib.api_destroy.argtypes = [ctypes.c_void_p]
    lib.api_destroy.restype = None

    # ── 3. api_chat (unified sync + stream) ──────────────────────────────────
    # stream_cb=NULL → sync (fills bot_reply_out). Non-NULL → tokens via callback.
    # context_json: per-request JSON context (user profile, etc.) or NULL.
    lib.api_chat.argtypes = [
        ctypes.c_void_p,   # ctx
        ctypes.c_char_p,   # tenant_id
        ctypes.c_char_p,   # user_id
        ctypes.c_char_p,   # user_message
        ctypes.c_char_p,   # context_json (NULL = no per-request context)
        ctypes.c_char_p,   # bot_reply_out
        c_size_t,           # out_size
        STREAM_TOKEN_CB,   # stream_cb (NULL for sync)
        ctypes.c_void_p,   # stream_userdata
    ]
    lib.api_chat.restype = ctypes.c_int

    # ── 4. api_load_chat_history(ctx, tenant_id, user_id) ────────────────────
    lib.api_load_chat_history.argtypes = [
        ctypes.c_void_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
    ]
    lib.api_load_chat_history.restype = ctypes.c_int

    # ── 5. api_get_history_message ───────────────────────────────────────────
    lib.api_get_history_message.argtypes = [
        ctypes.c_void_p, ctypes.c_int,
        ctypes.c_char_p, c_size_t, ctypes.c_char_p, c_size_t,
        ctypes.POINTER(ctypes.c_char),
        ctypes.c_char_p, c_size_t,
        ctypes.c_char_p, c_size_t,
    ]
    lib.api_get_history_message.restype = ctypes.c_int

    # ── 5b. api_greet (welcome message) ─────────────────────────────────────
    # Returns: 0 = greeting generated, 1 = no greeting needed, -1 = error
    lib.api_greet.argtypes = [
        ctypes.c_void_p,   # ctx
        ctypes.c_char_p,   # tenant_id
        ctypes.c_char_p,   # user_id
        ctypes.c_char_p,   # context_json (user info)
        ctypes.c_char_p,   # greet_opts_json (condition/response_type)
        ctypes.c_char_p,   # reply_out
        c_size_t,           # out_size
    ]
    lib.api_greet.restype = ctypes.c_int

    # ── 6. api_get_stats(ctx, api_stats_t *out) ─────────────────────────────
    lib.api_get_stats.argtypes = [ctypes.c_void_p, ctypes.POINTER(ApiStats)]
    lib.api_get_stats.restype = None

    # ── 7. api_geo_atlas_import_row ──────────────────────────────────────────
    lib.api_geo_atlas_import_row.argtypes = [
        ctypes.c_void_p,   # ctx
        ctypes.c_char_p,   # tenant_id
        ctypes.c_char_p,   # name
        ctypes.c_char_p,   # name_normalized
        ctypes.c_char_p,   # district
        ctypes.c_char_p,   # axis
        ctypes.c_char_p,   # category
        ctypes.c_char_p,   # city
        ctypes.POINTER(ctypes.c_float),  # vector (NULL ok)
        c_size_t,           # vector_dim
        ctypes.c_char_p,   # embed_model_id
        ctypes.c_char_p,   # source
        ctypes.c_char_p,   # verification_status
        ctypes.c_double,   # trust_score
    ]
    lib.api_geo_atlas_import_row.restype = ctypes.c_int

    # ── Build-time Ollama defaults (api_build.h — no I/O) ───────────────────
    lib.api_build_ollama_default_host.argtypes = []
    lib.api_build_ollama_default_host.restype = ctypes.c_char_p

    lib.api_build_ollama_default_port.argtypes = []
    lib.api_build_ollama_default_port.restype = ctypes.c_int

    lib.api_build_ollama_default_chat_model.argtypes = []
    lib.api_build_ollama_default_chat_model.restype = ctypes.c_char_p

    lib.api_build_ollama_default_embed_model.argtypes = []
    lib.api_build_ollama_default_embed_model.restype = ctypes.c_char_p

    lib.api_build_ollama_embed_max_dim.argtypes = []
    lib.api_build_ollama_embed_max_dim.restype = ctypes.c_int

    _loaded_lib = lib
    return lib


def _dec_utf8(raw) -> str:
    if raw is None:
        return ""
    if isinstance(raw, bytes):
        return raw.decode("utf-8", errors="replace")
    if isinstance(raw, int):
        if raw == 0:
            return ""
        return ctypes.string_at(raw).decode("utf-8", errors="replace").split("\0", 1)[0]
    return str(raw)


def c_build_ollama_default_host() -> str:
    return _dec_utf8(load_lib().api_build_ollama_default_host())


def c_build_ollama_default_port() -> int:
    return int(load_lib().api_build_ollama_default_port())


def c_build_ollama_default_chat_model() -> str:
    return _dec_utf8(load_lib().api_build_ollama_default_chat_model())


def c_build_ollama_default_embed_model() -> str:
    return _dec_utf8(load_lib().api_build_ollama_default_embed_model())


def c_build_ollama_embed_max_dim() -> int:
    return int(load_lib().api_build_ollama_embed_max_dim())
