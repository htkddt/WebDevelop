"""
Full options for python_ai: all api_options_t fields configurable via environment.
Use from run_ai_tui.py or any script that wants full control (training, tuning, etc.).
No default overrides here — env wins; missing env uses C library defaults (0/None).

``build_api_options*`` runs ``validate_api_options_dict`` before constructing ``ApiOptions`` so the
Python adapter rejects unsafe / inconsistent options instead of relying on c-lib silent fallbacks.
"""
import os
from typing import Any, Dict, List, Mapping, Optional, Tuple

# Import from parent so training can be run from python_ai root
import sys
_parent = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _parent not in sys.path:
    sys.path.insert(0, _parent)

from engine_ctypes import (
    M4ENGINE_MODE_ONLY_MEMORY,
    M4ENGINE_MODE_ONLY_MONGO,
    M4ENGINE_MODE_MONGO_REDIS,
    M4ENGINE_MODE_MONGO_REDIS_ELK,
    API_CONTEXT_BATCH_SIZE_DEFAULT,
)

_ENV_MODE = "M4ENGINE_MODE"
_ENV_MONGO_URI = "M4ENGINE_MONGO_URI"
_ENV_REDIS_HOST = "M4ENGINE_REDIS_HOST"
_ENV_REDIS_PORT = "M4ENGINE_REDIS_PORT"
_ENV_ES_HOST = "M4ENGINE_ES_HOST"
_ENV_ES_PORT = "M4ENGINE_ES_PORT"
_ENV_LOG_DB = "M4ENGINE_LOG_DB"
_ENV_LOG_COLL = "M4ENGINE_LOG_COLL"
_ENV_CONTEXT_BATCH_SIZE = "M4ENGINE_CONTEXT_BATCH_SIZE"
_ENV_SMART_TOPIC = "M4ENGINE_SMART_TOPIC"
_ENV_SMART_TOPIC_COLLECTION = "M4ENGINE_SMART_TOPIC_COLLECTION"
_ENV_SMART_TOPIC_MODEL_TINY = "M4ENGINE_SMART_TOPIC_MODEL_TINY"
_ENV_SMART_TOPIC_MODEL_B2 = "M4ENGINE_SMART_TOPIC_MODEL_B2"
_ENV_STACK_FALLBACK = "M4ENGINE_STACK_FALLBACK"
_ENV_LEARNING_TERMS_PATH = "M4_LEARNING_TERMS_PATH"
_ENV_ENABLE_LEARNING_TERMS = "M4_ENABLE_LEARNING_TERMS"
_ENV_DEFER_NL_LEARN_LOAD = "M4_DEFER_NL_LEARN_LOAD"
_ENV_DEFAULT_PERSONA = "M4_DEFAULT_PERSONA"
_ENV_DEFAULT_INSTRUCTIONS = "M4_DEFAULT_INSTRUCTIONS"
_ENV_DEFAULT_MODEL_LANE = "M4_DEFAULT_MODEL_LANE"
_ENV_GEO_AUTHORITY_CSV_PATH = "M4_GEO_AUTHORITY_CSV_PATH"
_ENV_GEO_MIGRATE_LEGACY = "M4_GEO_MIGRATE_LEGACY"
# All valid debug module keys from c-lib include/debug_log.h.
# Always enabled via api_create JSON — no env var needed.
ALL_DEBUG_MODULES = (
    "API", "ai_agent", "STORAGE", "GEO_LEARNING", "GEO_AUTH", "OLLAMA", "ELK",
    "EMBED_MIGRATION", "ENGINE", "CHAT", "nl_learn_terms", "LOGIC_CONFLICT",
)


def _env_enable_learning_terms() -> int:
    """Default **on** when env var is unset so ``server/data/nl_learn_terms.json`` updates without extra flags."""
    if _ENV_ENABLE_LEARNING_TERMS not in os.environ:
        return 1
    return 1 if _env_int(_ENV_ENABLE_LEARNING_TERMS, 0) else 0


def _env_defer_learning_terms_load() -> int:
    """Default **1**: c-lib loads ``learning_terms_path`` in a background pthread. ``M4_DEFER_NL_LEARN_LOAD=0`` = sync open."""
    if _ENV_DEFER_NL_LEARN_LOAD not in os.environ:
        return 1
    return 1 if _env_int(_ENV_DEFER_NL_LEARN_LOAD, 0) else 0


def _inject_nl_learn_terms_default_path(opts_dict: dict) -> None:
    """If ``learning_terms_path`` is empty, use ``server/data/nl_learn_terms.json`` (see ``nl_learn_terms_bridge``)."""
    if (opts_dict.get("learning_terms_path") or "").strip():
        return
    try:
        from server.nl_learn_terms_bridge import default_nl_learn_terms_data_path
    except ImportError:
        try:
            from nl_learn_terms_bridge import default_nl_learn_terms_data_path
        except ImportError:
            return
    opts_dict["learning_terms_path"] = default_nl_learn_terms_data_path()

# Matches ``API_CTX_CAPACITY_MAX`` in c-lib ``src/api.c`` (ring / context window cap).
_CLIB_CONTEXT_BATCH_MAX = 64

# Keys persisted in app DB and merged on top of env/max defaults before ``api_create``.
OPTION_KEYS = frozenset(
    {
        "mode",
        "mongo_uri",
        "redis_host",
        "redis_port",
        "es_host",
        "es_port",
        "log_db",
        "log_coll",
        "context_batch_size",
        "smart_topic",
        "smart_topic_collection",
        "smart_topic_model_tiny",
        "smart_topic_model_b2",
        # SharedCollection: ``api_options_t`` only (``.cursor/shared_collection.md``); optional admin DB overrides.
        "shared_collection_json_path",
        "shared_collection_mongo_uri",
        "shared_collection_backfill_db",
        "learning_terms_path",
        "enable_learning_terms",
        "defer_learning_terms_load",
    }
)

_LOG_ENGINE_RESOLVED_ENV = "M4_LOG_ENGINE_RESOLVED_OPTIONS"


def log_resolved_engine_options_at_startup(
    opts_dict: Mapping[str, Any], *, history_size: int, source: str
) -> None:
    """
    Stderr: full ``OPTION_KEYS`` snapshot as passed into ``build_api_options*`` / ``api_create`` (values, not env names).
    Mongo URIs are redacted to lengths only. Disable: ``M4_LOG_ENGINE_RESOLVED_OPTIONS=0|false|no|off``.
    """
    flag = os.environ.get(_LOG_ENGINE_RESOLVED_ENV, "1").strip().lower()
    if flag in ("0", "false", "no", "off"):
        return
    print(
        f"[M4] engine init — full resolved options (Python→api_create) source={source!r} "
        f"history_ring={history_size}",
        file=sys.stderr,
    )
    _uri_keys = frozenset({"mongo_uri", "shared_collection_mongo_uri"})
    for key in sorted(OPTION_KEYS):
        val = opts_dict.get(key)
        if val is None or val == "":
            disp = "<empty>"
        elif key in _uri_keys and isinstance(val, str):
            disp = f"<set, {len(val)} chars>" if val else "<empty>"
        elif isinstance(val, str) and len(val) > 120:
            disp = repr(val[:117] + "...")
        else:
            disp = repr(val)
        print(f"[M4]   {key}={disp}", file=sys.stderr)
    print(
        "[M4]   (ctypes ApiOptions fields fixed by Python adapter: inject_geo_knowledge=0, vector_gen_backend=0, "
        "embed_migration_autostart=0, session_idle_seconds=0, model_switch_opts=NULL, vector_ollama_model=NULL — "
        "see c-lib [API] api_create for binary layout / strlen totals)",
        file=sys.stderr,
    )


# UI + ``GET /api/admin/bot-c-lib/schema`` — documents every tunable field (see ``get_full_options`` / ``get_max_options``).
BOT_C_LIB_OPTION_SCHEMA: List[Dict[str, Any]] = [
    {
        "key": "mode",
        "env": _ENV_MODE,
        "type": "int",
        "min": 0,
        "max": 3,
        "label": "Mode",
        "help": "0 MEMORY only · 1 Mongo · 2 Mongo+Redis · 3 Mongo+Redis+ELK. c-lib only uses es_host when mode is 3; "
        "Python build_api_options* promotes mode to 3 if es_host is non-empty.",
    },
    {
        "key": "mongo_uri",
        "env": _ENV_MONGO_URI,
        "type": "string",
        "label": "MongoDB URI",
        "help": "Chat history / persistence URI for c-lib mongoc.",
    },
    {
        "key": "redis_host",
        "env": _ENV_REDIS_HOST,
        "type": "string",
        "label": "Redis host",
        "help": "L2 cache host (c-lib stub unless Redis implemented).",
    },
    {
        "key": "redis_port",
        "env": _ENV_REDIS_PORT,
        "type": "int",
        "min": 0,
        "max": 65535,
        "label": "Redis port",
        "help": "0 may mean default client behavior in c-lib.",
    },
    {
        "key": "es_host",
        "env": _ENV_ES_HOST,
        "type": "string",
        "label": "Elasticsearch host",
        "help": "Elasticsearch HTTP host; only used when mode is MONGO_REDIS_ELK (3). Non-empty value auto-sets mode 3 in Python.",
    },
    {
        "key": "es_port",
        "env": _ENV_ES_PORT,
        "type": "int",
        "min": 0,
        "max": 65535,
        "label": "Elasticsearch port",
        "help": "HTTP port for Elasticsearch.",
    },
    {
        "key": "log_db",
        "env": _ENV_LOG_DB,
        "type": "string",
        "label": "Log database name",
        "help": "Optional logging DB name passed to c-lib.",
    },
    {
        "key": "log_coll",
        "env": _ENV_LOG_COLL,
        "type": "string",
        "label": "Log collection name",
        "help": "Optional logging collection.",
    },
    {
        "key": "context_batch_size",
        "env": _ENV_CONTEXT_BATCH_SIZE,
        "type": "int",
        "min": 0,
        "max": _CLIB_CONTEXT_BATCH_MAX,
        "label": "Context batch size",
        "help": f"History turns in context; 0 uses c-lib default (30). Max {_CLIB_CONTEXT_BATCH_MAX} (API_CTX_CAPACITY_MAX).",
    },
    {
        "key": "smart_topic",
        "env": _ENV_SMART_TOPIC,
        "type": "bool_int",
        "label": "Smart topic",
        "help": "Enable smart_topic_opts (mini-AI topic routing) when non-zero.",
    },
    {
        "key": "smart_topic_collection",
        "env": _ENV_SMART_TOPIC_COLLECTION,
        "type": "string",
        "label": "Smart topic Mongo collection",
        "help": "Collection name for smart topic feature.",
    },
    {
        "key": "smart_topic_model_tiny",
        "env": _ENV_SMART_TOPIC_MODEL_TINY,
        "type": "string",
        "label": "Smart topic model (tiny)",
        "help": "Ollama/model id for tiny lane.",
    },
    {
        "key": "smart_topic_model_b2",
        "env": _ENV_SMART_TOPIC_MODEL_B2,
        "type": "string",
        "label": "Smart topic model (b2)",
        "help": "Ollama/model id for b2 lane.",
    },
    {
        "key": "shared_collection_json_path",
        "env": "",
        "type": "string",
        "label": "SharedCollection registry JSON path",
        "help": "Absolute path to registry JSON (see ``.cursor/shared_collection.md`` section 2). Default: generated from "
        "``server/shared_collection_catalog`` when mode is ELK and es_host is set. Override via admin only (no process env).",
    },
    {
        "key": "shared_collection_mongo_uri",
        "env": "",
        "type": "string",
        "label": "SharedCollection Mongo URI",
        "help": "Optional separate Mongo cluster for SC backfill/validation; default matches chat mongo_uri. "
        "Must start with mongodb:// or mongodb+srv://.",
    },
    {
        "key": "shared_collection_backfill_db",
        "env": "",
        "type": "string",
        "label": "SharedCollection backfill database",
        "help": "Mongo database for SC collections (default ``product`` for store catalog).",
    },
    {
        "key": "learning_terms_path",
        "env": _ENV_LEARNING_TERMS_PATH,
        "type": "string",
        "label": "NL learn terms TSV path",
        "help": "Filesystem path for term→intent counts (JSON v2 preferred, TSV v1 supported, c-lib ``nl_learn_terms``). Empty = disabled. "
        "Backend-agnostic priors for NL routing (see ``.cursor/elk_nl_routing.md`` §8).",
    },
    {
        "key": "enable_learning_terms",
        "env": _ENV_ENABLE_LEARNING_TERMS,
        "type": "bool_int",
        "label": "Enable NL term learning writes",
        "help": "Non-zero: allow incrementing counts and persisting to ``learning_terms_path``. Zero: load/score only.",
    },
]

# API/UI: mode int → label (derived from c-lib constants imported above; no duplicate literals in admin routes).
BOT_C_LIB_MODE_LABELS: Dict[str, str] = {
    str(M4ENGINE_MODE_ONLY_MEMORY): "MEMORY",
    str(M4ENGINE_MODE_ONLY_MONGO): "MONGO_ONLY",
    str(M4ENGINE_MODE_MONGO_REDIS): "MONGO_REDIS",
    str(M4ENGINE_MODE_MONGO_REDIS_ELK): "MONGO_REDIS_ELK",
}


def _env_int(key: str, default: Optional[int] = None) -> Optional[int]:
    v = os.environ.get(key)
    if v is None or v == "":
        return default
    try:
        return int(v)
    except ValueError:
        return default


def _env_str(key: str, default: Optional[str] = None) -> Optional[str]:
    v = os.environ.get(key)
    if v is None or v == "":
        return default
    return v


def _inject_shared_collection_defaults_from_registry(opts_dict: dict) -> None:
    """
    When mode is MONGO_REDIS_ELK and ``es_host`` is set, fill SharedCollection fields from
    ``.cursor/shared_collection.md``-style registry (``server/shared_collection_catalog``) and store DB ``product``.
    No ``M4_SHARED_COLLECTION_*`` or ``M4ENGINE_SHARED_COLLECTION_*`` env vars — only ``api_options_t`` / merged dict.
    """
    try:
        m = int(opts_dict.get("mode") or 0)
    except (TypeError, ValueError):
        return
    if m != M4ENGINE_MODE_MONGO_REDIS_ELK:
        return
    es = opts_dict.get("es_host")
    if not es or not str(es).strip():
        return
    if not (opts_dict.get("shared_collection_json_path") or "").strip():
        try:
            from server.shared_collection_catalog import ensure_shared_collection_registry_json_file
        except ImportError:
            try:
                from shared_collection_catalog import ensure_shared_collection_registry_json_file
            except ImportError:
                return
        opts_dict["shared_collection_json_path"] = ensure_shared_collection_registry_json_file()
    if not (opts_dict.get("shared_collection_backfill_db") or "").strip():
        opts_dict["shared_collection_backfill_db"] = "product"
    if not (opts_dict.get("shared_collection_mongo_uri") or "").strip():
        mu = opts_dict.get("mongo_uri")
        if mu and str(mu).strip():
            opts_dict["shared_collection_mongo_uri"] = str(mu).strip()


def get_full_options() -> dict:
    """
    Read all options from environment (full option set).
    Keys: mode, mongo_uri, redis_host, redis_port, es_host, es_port, log_db, log_coll, context_batch_size,
    smart_topic, smart_topic_collection, smart_topic_model_tiny, smart_topic_model_b2.
    Values are raw (int/str/None); use build_api_options() to get (ApiOptions, history_size, smart_topic_keepalive).
    """
    mode_raw = _env_int(_ENV_MODE)
    if mode_raw is not None and 0 <= mode_raw <= 3:
        mode = mode_raw
    else:
        mode = M4ENGINE_MODE_ONLY_MEMORY

    smart_topic = _env_int(_ENV_SMART_TOPIC, 0)
    d = {
        "mode": mode,
        "mongo_uri": _env_str(_ENV_MONGO_URI),
        "redis_host": _env_str(_ENV_REDIS_HOST),
        "redis_port": _env_int(_ENV_REDIS_PORT, 0),
        "es_host": _env_str(_ENV_ES_HOST),
        "es_port": _env_int(_ENV_ES_PORT, 0),
        "log_db": _env_str(_ENV_LOG_DB),
        "log_coll": _env_str(_ENV_LOG_COLL),
        "context_batch_size": _env_int(_ENV_CONTEXT_BATCH_SIZE, 0) or 0,
        "smart_topic": 1 if smart_topic else 0,
        "smart_topic_collection": _env_str(_ENV_SMART_TOPIC_COLLECTION),
        "smart_topic_model_tiny": _env_str(_ENV_SMART_TOPIC_MODEL_TINY),
        "smart_topic_model_b2": _env_str(_ENV_SMART_TOPIC_MODEL_B2),
        "shared_collection_json_path": None,
        "shared_collection_mongo_uri": None,
        "shared_collection_backfill_db": None,
        "learning_terms_path": _env_str(_ENV_LEARNING_TERMS_PATH),
        "enable_learning_terms": _env_enable_learning_terms(),
        "defer_learning_terms_load": _env_defer_learning_terms_load(),
        "default_persona": _env_str(_ENV_DEFAULT_PERSONA),
        "default_instructions": _env_str(_ENV_DEFAULT_INSTRUCTIONS),
        "default_model_lane": _env_int(_ENV_DEFAULT_MODEL_LANE, 0) or 0,
        "geo_authority_csv_path": _env_str(_ENV_GEO_AUTHORITY_CSV_PATH),
        "geo_migrate_legacy": _env_int(_ENV_GEO_MIGRATE_LEGACY, 0) or 0,
    }
    _coerce_mode_to_elk_if_es_host_set(d)
    _inject_shared_collection_defaults_from_registry(d)
    _inject_nl_learn_terms_default_path(d)
    return d


# Defaults for max option (Mongo + Redis + ELK full stack)
_DEFAULT_MONGO_URI = "mongodb://127.0.0.1:27017"
_DEFAULT_REDIS_HOST = "127.0.0.1"
_DEFAULT_REDIS_PORT = 6379
_DEFAULT_ES_HOST = "127.0.0.1"
_DEFAULT_ES_PORT = 9200


def get_max_options() -> dict:
    """
    Max option: mode=MONGO_REDIS_ELK with MongoDB, Redis, ELK enabled.
    Env vars override defaults (M4ENGINE_MONGO_URI, M4ENGINE_REDIS_HOST, etc.).
    """
    smart_topic = _env_int(_ENV_SMART_TOPIC, 0)
    d = {
        "mode": M4ENGINE_MODE_MONGO_REDIS_ELK,
        "mongo_uri": _env_str(_ENV_MONGO_URI) or _DEFAULT_MONGO_URI,
        "redis_host": _env_str(_ENV_REDIS_HOST) or _DEFAULT_REDIS_HOST,
        "redis_port": _env_int(_ENV_REDIS_PORT) or _DEFAULT_REDIS_PORT,
        "es_host": _env_str(_ENV_ES_HOST) or _DEFAULT_ES_HOST,
        "es_port": _env_int(_ENV_ES_PORT) or _DEFAULT_ES_PORT,
        "log_db": _env_str(_ENV_LOG_DB),
        "log_coll": _env_str(_ENV_LOG_COLL),
        "context_batch_size": _env_int(_ENV_CONTEXT_BATCH_SIZE, 0) or 0,
        "smart_topic": 1 if smart_topic else 0,
        "smart_topic_collection": _env_str(_ENV_SMART_TOPIC_COLLECTION),
        "smart_topic_model_tiny": _env_str(_ENV_SMART_TOPIC_MODEL_TINY),
        "smart_topic_model_b2": _env_str(_ENV_SMART_TOPIC_MODEL_B2),
        "shared_collection_json_path": None,
        "shared_collection_mongo_uri": None,
        "shared_collection_backfill_db": None,
        "learning_terms_path": _env_str(_ENV_LEARNING_TERMS_PATH),
        "enable_learning_terms": _env_enable_learning_terms(),
        "defer_learning_terms_load": _env_defer_learning_terms_load(),
        "default_persona": _env_str(_ENV_DEFAULT_PERSONA),
        "default_instructions": _env_str(_ENV_DEFAULT_INSTRUCTIONS),
        "default_model_lane": _env_int(_ENV_DEFAULT_MODEL_LANE, 0) or 0,
        "geo_authority_csv_path": _env_str(_ENV_GEO_AUTHORITY_CSV_PATH),
        "geo_migrate_legacy": _env_int(_ENV_GEO_MIGRATE_LEGACY, 0) or 0,
    }
    _inject_shared_collection_defaults_from_registry(d)
    _inject_nl_learn_terms_default_path(d)
    return d


def build_snapshot_bot_c_lib_values_for_db(*, use_max: bool) -> Dict[str, Any]:
    """
    All ``OPTION_KEYS`` from the current environment for persisting in ``bot_c_lib_settings``.
    ``use_max=True`` matches ``get_max_options()`` (stack defaults + env); ``False`` matches
    ``get_full_options()`` (env-only, memory default mode).
    """
    base = get_max_options() if use_max else get_full_options()
    return {k: base[k] for k in sorted(OPTION_KEYS)}


def _schema_row_for_key(key: str) -> Optional[Dict[str, Any]]:
    for row in BOT_C_LIB_OPTION_SCHEMA:
        if row.get("key") == key:
            return row
    return None


def _coerce_option_value(key: str, value: Any) -> Any:
    row = _schema_row_for_key(key)
    st = str((row or {}).get("type") or "string")
    if st == "int":
        if value is None or value == "":
            return 0
        if isinstance(value, bool):
            return int(value)
        x = int(value)
        mn = (row or {}).get("min")
        mx = (row or {}).get("max")
        if mn is not None and x < int(mn):
            raise ValueError(f"{key} below minimum {mn}")
        if mx is not None and x > int(mx):
            raise ValueError(f"{key} above maximum {mx}")
        return x
    if st == "bool_int":
        if isinstance(value, str):
            return 1 if value.strip().lower() in ("1", "true", "yes", "on") else 0
        return 1 if value else 0
    if value is None:
        return None
    s = str(value).strip()
    return s or None


def merge_options_overrides(base: dict, overrides: Optional[Dict[str, Any]]) -> dict:
    """Shallow merge: only ``OPTION_KEYS``; ``None`` / missing values in ``overrides`` are skipped."""
    out = dict(base)
    if not overrides:
        return out
    for k, v in overrides.items():
        if k not in OPTION_KEYS:
            continue
        if v is None:
            continue
        out[k] = _coerce_option_value(k, v)
    return out


def _mongo_conn_uri_ok(s: str) -> bool:
    t = str(s).strip()
    if not t:
        return True
    return t.startswith("mongodb://") or t.startswith("mongodb+srv://")


def validate_api_options_dict(opts_dict: dict) -> None:
    """
    Reject ``api_options_t``-shaped dicts that c-lib would mis-handle (silent mode remap, bad ports,
    ctypes hazards). Call **after** ``_coerce_mode_to_elk_if_es_host_set``. Raises ``ValueError``
    with a human-readable message so the Python harness fails fast before ``api_create``.
    """
    errors: List[str] = []

    raw_mode = opts_dict.get("mode")
    if isinstance(raw_mode, bool):
        errors.append("mode must be an integer 0..3, not a boolean")
        m: Optional[int] = None
    else:
        try:
            m = int(raw_mode)
        except (TypeError, ValueError):
            errors.append(f"mode must be an integer 0..3, got {raw_mode!r}")
            m = None
        else:
            if m < 0 or m > 3:
                errors.append(
                    f"mode={m} is out of range 0..3 (c-lib api_create would silently use M4ENGINE_MODE_ONLY_MONGO)"
                )
                m = None

    def _port(name: str, raw: Any) -> None:
        try:
            p = int(raw or 0)
        except (TypeError, ValueError):
            errors.append(f"{name} must be an integer 0..65535, got {raw!r}")
            return
        if p < 0 or p > 65535:
            errors.append(f"{name} must be an integer 0..65535, got {p}")

    _port("redis_port", opts_dict.get("redis_port"))
    _port("es_port", opts_dict.get("es_port"))

    try:
        cbs = int(opts_dict.get("context_batch_size") or 0)
    except (TypeError, ValueError):
        errors.append(f"context_batch_size must be an integer, got {opts_dict.get('context_batch_size')!r}")
    else:
        if cbs < 0:
            errors.append(f"context_batch_size must be >= 0, got {cbs}")
        elif cbs > _CLIB_CONTEXT_BATCH_MAX:
            errors.append(
                f"context_batch_size={cbs} exceeds c-lib maximum {_CLIB_CONTEXT_BATCH_MAX} (API_CTX_CAPACITY_MAX)"
            )

    if m is not None:
        es = opts_dict.get("es_host")
        es_set = es is not None and bool(str(es).strip())
        if es_set and m != M4ENGINE_MODE_MONGO_REDIS_ELK:
            errors.append(
                f"non-empty es_host requires mode={M4ENGINE_MODE_MONGO_REDIS_ELK} (MONGO_REDIS_ELK); got mode={m}"
            )

    for key in (
        "mongo_uri",
        "redis_host",
        "es_host",
        "log_db",
        "log_coll",
        "smart_topic_collection",
        "smart_topic_model_tiny",
        "smart_topic_model_b2",
        "shared_collection_json_path",
        "shared_collection_mongo_uri",
        "shared_collection_backfill_db",
        "learning_terms_path",
    ):
        val = opts_dict.get(key)
        if val is None:
            continue
        if isinstance(val, str) and "\x00" in val:
            errors.append(f"{key} must not contain NUL bytes (unsafe for c-lib ctypes strings)")

    ltp = opts_dict.get("learning_terms_path")
    if ltp is not None and str(ltp).strip():
        s = str(ltp)
        if len(s) > 4096:
            errors.append("learning_terms_path exceeds c-lib maximum length 4096 (m4_validate_optional_path_string)")
        for ch in ("\t", "\n", "\r"):
            if ch in s:
                errors.append("learning_terms_path must not contain TAB or newline characters")

    elt = opts_dict.get("enable_learning_terms")
    if elt is not None and not isinstance(elt, bool):
        try:
            e = int(elt)
        except (TypeError, ValueError):
            errors.append(f"enable_learning_terms must be 0/1 or bool-coercible int, got {elt!r}")
        else:
            if e not in (0, 1):
                errors.append(f"enable_learning_terms must be 0 or 1, got {e}")

    dlt = opts_dict.get("defer_learning_terms_load")
    if dlt is not None and not isinstance(dlt, bool):
        try:
            dd = int(dlt)
        except (TypeError, ValueError):
            errors.append(f"defer_learning_terms_load must be 0/1 or bool-coercible int, got {dlt!r}")
        else:
            if dd not in (0, 1):
                errors.append(f"defer_learning_terms_load must be 0 or 1, got {dd}")

    st = opts_dict.get("smart_topic")
    if st is not None and not isinstance(st, bool):
        try:
            int(st)
        except (TypeError, ValueError):
            errors.append(f"smart_topic must be 0/1 or bool-coercible int, got {st!r}")

    if m is not None and m >= 1:
        mu = opts_dict.get("mongo_uri")
        if mu is not None and str(mu).strip() and not _mongo_conn_uri_ok(str(mu)):
            errors.append("mongo_uri must start with mongodb:// or mongodb+srv:// when set")

    scu = opts_dict.get("shared_collection_mongo_uri")
    if scu is not None and str(scu).strip() and not _mongo_conn_uri_ok(str(scu)):
        errors.append(
            "shared_collection_mongo_uri must start with mongodb:// or mongodb+srv:// when set"
        )

    if errors:
        raise ValueError("; ".join(errors))


def _apply_mode_stack_fields(out: dict, m: int) -> None:
    """Strip higher-tier fields so the dict matches c-lib ``fill_default_config`` for ``mode``."""
    out["mode"] = m
    if m <= 0:
        out["mongo_uri"] = None
        out["redis_host"] = None
        out["redis_port"] = 0
        out["es_host"] = None
        out["es_port"] = 0
    elif m == 1:
        out["redis_host"] = None
        out["redis_port"] = 0
        out["es_host"] = None
        out["es_port"] = 0
    elif m == 2:
        out["es_host"] = None
        out["es_port"] = 0


def apply_stack_fallback(
    opts_dict: dict,
    *,
    mongo_ok: bool,
    redis_ok: bool,
    elk_ok: bool,
) -> Tuple[dict, List[str]]:
    """
    When the desired mode is higher than infrastructure allows, downgrade **HIGH → LOW**:
    MONGO_REDIS_ELK (3) → MONGO_REDIS (2) if ELK down; → ONLY_MONGO (1) if Redis down; → ONLY_MEMORY (0)
    if Mongo down. Clears incompatible host/URI fields for the effective mode (Python adapter only — **does not**
    modify c-lib source).
    """
    out = dict(opts_dict)
    reasons: List[str] = []
    try:
        m = int(out.get("mode"))
    except (TypeError, ValueError):
        m = 0
    if m < 0 or m > 3:
        m = 0
    orig = m
    if m >= 1 and not mongo_ok:
        m = 0
        reasons.append("Mongo unreachable; falling back to M4ENGINE_MODE_ONLY_MEMORY (0)")
    elif m >= 2 and not redis_ok:
        m = 1
        reasons.append("Redis unreachable; falling back to M4ENGINE_MODE_ONLY_MONGO (1)")
    elif m >= 3 and not elk_ok:
        m = 2
        reasons.append("Elasticsearch unreachable; falling back to M4ENGINE_MODE_MONGO_REDIS (2)")
    if m != orig:
        _apply_mode_stack_fields(out, m)
    return out, reasons


def _coerce_mode_to_elk_if_es_host_set(opts_dict: dict) -> None:
    """
    c-lib only wires Elasticsearch when execution mode is MONGO_REDIS_ELK (mode 3);
    lower modes clear es_host in fill_default_config. If the caller set a non-empty
    es_host, promote mode to 3 so ELK and [ELK flow] logs match intent.
    """
    es = opts_dict.get("es_host")
    if es is None or (isinstance(es, str) and not str(es).strip()):
        return
    try:
        m = int(opts_dict["mode"])
    except (TypeError, ValueError, KeyError):
        m = 0
    if m < M4ENGINE_MODE_MONGO_REDIS_ELK:
        opts_dict["mode"] = M4ENGINE_MODE_MONGO_REDIS_ELK


def normalize_stored_overrides(raw: Optional[Dict[str, Any]]) -> Dict[str, Any]:
    """Validate and coerce a user-provided map for persistence (DB). Drops unknown keys and nulls."""
    if not raw:
        return {}
    out: Dict[str, Any] = {}
    for k, v in raw.items():
        if k not in OPTION_KEYS:
            continue
        if v is None or v == "":
            continue
        out[k] = _coerce_option_value(k, v)
    return out


def validate_client_bot_c_lib_values(values: dict) -> None:
    """
    Strict validation for **client-sent** ``values`` (admin PUT): only ``OPTION_KEYS`` allowed,
    each value must coerce per schema. Unknown keys **fail** (do not silently drop — that is only
    for legacy ``normalize_stored_overrides`` reads).
    """
    if not isinstance(values, dict):
        raise ValueError("values must be an object")
    for k in values:
        if k not in OPTION_KEYS:
            raise ValueError(
                f"unknown key {k!r}; allowed keys are training.full_options.OPTION_KEYS only"
            )
    for k, v in values.items():
        if v is None:
            continue
        try:
            _coerce_option_value(k, v)
        except ValueError as e:
            raise ValueError(f"invalid {k}: {e}") from e


def _build_api_options_json(opts_dict: dict) -> Tuple[str, int]:
    """Convert options dict to JSON string for ``api_create(json)``. Returns (json_str, history_size)."""
    import json as _json
    opts_dict = dict(opts_dict)
    _coerce_mode_to_elk_if_es_host_set(opts_dict)
    validate_api_options_dict(opts_dict)
    ctx_size = opts_dict.get("context_batch_size") or 0
    history_size = ctx_size if ctx_size > 0 else API_CONTEXT_BATCH_SIZE_DEFAULT

    # Map Python dict keys to c-lib JSON keys (strip None values)
    json_obj: Dict[str, Any] = {}
    # Direct pass-through keys
    _direct_keys = [
        "mode", "mongo_uri", "redis_host", "redis_port",
        "es_host", "es_port", "log_db", "log_coll",
        "context_batch_size", "inject_geo_knowledge",
        "disable_auto_system_time", "geo_authority",
        "vector_gen_backend", "vector_ollama_model",
        "embed_migration_autostart", "session_idle_seconds",
        "shared_collection_mongo_uri", "shared_collection_json_path",
        "shared_collection_backfill_db",
        "learning_terms_path", "enable_learning_terms",
        "defer_learning_terms_load",
        "default_persona", "default_instructions", "default_model_lane",
        "geo_authority_csv_path", "geo_migrate_legacy",
    ]
    for k in _direct_keys:
        v = opts_dict.get(k)
        if v is not None and v != "" and v != 0:
            json_obj[k] = v
    # mode=0 is valid (MEMORY)
    if opts_dict.get("mode") == 0:
        json_obj["mode"] = 0

    # Always enable all debug modules (c-lib debug_log.h)
    json_obj["debug_modules"] = list(ALL_DEBUG_MODULES)

    # smart_topic → inline fields
    if opts_dict.get("smart_topic"):
        st_coll = opts_dict.get("smart_topic_collection") or "smart_topic"
        json_obj["smart_topic"] = {
            "enable": 1,
            "collection": st_coll,
        }
        mt = opts_dict.get("smart_topic_model_tiny")
        mb = opts_dict.get("smart_topic_model_b2")
        if mt:
            json_obj["smart_topic"]["model_tiny"] = mt
        if mb:
            json_obj["smart_topic"]["model_b2"] = mb

    return _json.dumps(json_obj, ensure_ascii=False), history_size


def _build_api_options_from_dict(opts_dict: dict) -> Tuple[str, int, None]:
    """Build JSON config string for ``api_create``. Returns (json_str, history_size, None)."""
    json_str, history_size = _build_api_options_json(opts_dict)
    return json_str, history_size, None


def build_api_options() -> Tuple[str, int, None]:
    """Build JSON config from env (``get_full_options``) for ``api_create``."""
    return _build_api_options_from_dict(get_full_options())


def build_api_options_with_overrides(overrides: Optional[Dict[str, Any]]) -> Tuple[str, int, None]:
    """Like ``build_api_options`` but merges persisted overrides (e.g. from app Mongo) on top of env."""
    return _build_api_options_from_dict(merge_options_overrides(get_full_options(), overrides))


def build_max_api_options() -> Tuple[str, int, None]:
    """Build JSON config from env + max defaults (``get_max_options``) for ``api_create``."""
    return _build_api_options_from_dict(get_max_options())


def build_max_api_options_with_overrides(overrides: Optional[Dict[str, Any]]) -> Tuple[str, int, None]:
    """Like ``build_max_api_options`` with DB overrides merged on top."""
    return _build_api_options_from_dict(merge_options_overrides(get_max_options(), overrides))


def resolve_engine_options_dict(
    use_max: bool,
    overrides: Optional[Dict[str, Any]],
    *,
    apply_fallback: Optional[bool] = None,
    connectivity: Optional[Mapping[str, bool]] = None,
) -> Tuple[dict, List[str]]:
    """
    Merge env base with overrides, then optionally apply **stack fallback** (see ``apply_stack_fallback``)
    using live probes (``training.stack_connectivity``) unless ``connectivity`` is provided (tests).

    Disable fallback: ``M4ENGINE_STACK_FALLBACK=0`` or ``apply_fallback=False``.
    """
    base = get_max_options() if use_max else get_full_options()
    merged = merge_options_overrides(base, overrides or {})
    _inject_shared_collection_defaults_from_registry(merged)
    _inject_nl_learn_terms_default_path(merged)
    if apply_fallback is None:
        apply_fallback = os.environ.get(_ENV_STACK_FALLBACK, "1").strip().lower() not in (
            "0",
            "false",
            "no",
        )
    if not apply_fallback:
        return merged, []
    if connectivity is not None:
        conn = {
            "mongo_ok": bool(connectivity.get("mongo_ok")),
            "redis_ok": bool(connectivity.get("redis_ok")),
            "elk_ok": bool(connectivity.get("elk_ok")),
        }
    else:
        from training.stack_connectivity import probe_stack_connectivity_from_merged

        conn = probe_stack_connectivity_from_merged(merged)
    return apply_stack_fallback(
        merged,
        mongo_ok=conn["mongo_ok"],
        redis_ok=conn["redis_ok"],
        elk_ok=conn["elk_ok"],
    )


def build_api_options_from_resolved_dict(opts_dict: dict) -> Tuple[str, int, None]:
    """Build JSON config from a merged dict (after optional ``resolve_engine_options_dict``) for ``api_create``."""
    return _build_api_options_from_dict(opts_dict)
