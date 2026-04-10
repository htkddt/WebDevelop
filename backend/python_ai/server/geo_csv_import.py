# -*- coding: utf-8 -*-
"""
CSV → geo_atlas via c-lib ``api_geo_atlas_import_row``. Optional vectors: precomputed ``vector``
column, or ``embed=1`` on the HTTP route so Flask calls c-lib ``api_embed_text`` (no Python HTTP to Ollama).

Default CSV columns (header row): name, name_normalized, district, axis, category, city,
source, verification_status, trust_score, vector (JSON array of floats, optional),
embed_text (optional; passed to ``api_embed_text`` when vector empty).

Dynamic mapping: pass JSON `mapping` { "logical_name": "YourHeader" } to rename columns.
"""
from __future__ import annotations

import csv
import ctypes
import io
import json
import os
import sys
from typing import Any, Dict, List, Optional, Tuple

_py_ai_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _py_ai_root not in sys.path:
    sys.path.insert(0, _py_ai_root)

# Logical field → default CSV header name
DEFAULT_HEADER_MAP: Dict[str, str] = {
    "name": "name",
    "name_normalized": "name_normalized",
    "district": "district",
    "axis": "axis",
    "category": "category",
    "city": "city",
    "source": "source",
    "verification_status": "verification_status",
    "trust_score": "trust_score",
    "vector": "vector",
    "embed_text": "embed_text",
}

# If the primary header (from mapping / default) is missing or empty, try these (e.g. docs/csv/geo.csv).
HEADER_FALLBACKS: Dict[str, Tuple[str, ...]] = {
    "name": ("name", "location_name", "place", "landmark", "title", "location"),
    "name_normalized": ("name_normalized", "name_norm", "slug"),
    "district": ("district", "province", "quan", "state"),
    "axis": ("axis", "region", "area"),
    "category": ("category", "type", "kind"),
    "city": ("city", "capital", "center", "seat"),
    "source": ("source",),
    "verification_status": ("verification_status", "status"),
    "trust_score": ("trust_score", "trust"),
    "vector": ("vector", "embedding"),
    "embed_text": ("embed_text", "text_for_embed", "prompt"),
}


def _norm_name(s: str) -> str:
    return "_".join(s.strip().lower().split())


def parse_geo_csv(
    text: str,
    mapping: Optional[Dict[str, str]] = None,
) -> Tuple[List[Dict[str, Any]], List[Dict[str, Any]]]:
    """
    Parse UTF-8 CSV text into row dicts ready for api_geo_atlas_import_row.
    `mapping`: logical field name -> your CSV column header (dynamic import).

    Returns (rows, errors) where each row has: name, name_normalized, district, axis,
    category, city, source, verification_status, trust_score, embedding (list[float]|None), embed_text.
    """
    merged: Dict[str, str] = dict(DEFAULT_HEADER_MAP)
    if mapping:
        merged.update(mapping)

    f = io.StringIO(text.lstrip("\ufeff"))
    reader = csv.DictReader(f)
    if not reader.fieldnames:
        return [], [{"row": 0, "error": "no header row"}]

    rows: List[Dict[str, Any]] = []
    errors: List[Dict[str, Any]] = []

    def get_cell(raw: Dict[str, Any], logical: str) -> str:
        want = merged.get(logical, logical)
        candidates = (want,) + HEADER_FALLBACKS.get(logical, ())
        seen = set()
        ordered = []
        for c in candidates:
            cl = c.lower()
            if cl not in seen:
                seen.add(cl)
                ordered.append(c)
        for cand in ordered:
            for key in raw:
                if key and key.strip().lower() == cand.lower():
                    v = raw[key]
                    return v.strip() if v else ""
        return ""

    def raw_get_any(raw: Dict[str, Any], headers: Tuple[str, ...]) -> str:
        for h in headers:
            for key in raw:
                if key and key.strip().lower() == h.lower():
                    v = raw[key]
                    if v and str(v).strip():
                        return str(v).strip()
        return ""

    for i, raw in enumerate(reader, start=2):
        def get(logical: str) -> str:
            return get_cell(raw, logical)

        name = get("name")
        if not name:
            continue

        vec_raw = get("vector")
        embedding: Optional[List[float]] = None
        if vec_raw:
            try:
                embedding = json.loads(vec_raw)
                if not isinstance(embedding, list):
                    raise ValueError("vector must be a JSON array")
                embedding = [float(x) for x in embedding]
            except (json.JSONDecodeError, ValueError, TypeError) as e:
                errors.append({"row": i, "error": f"vector: {e}"})
                continue

        ts = get("trust_score")
        trust = 1.0
        if ts:
            try:
                trust = float(ts)
            except ValueError:
                trust = 1.0

        nn = get("name_normalized") or _norm_name(name)
        embed_text = get("embed_text")
        if not embed_text:
            chunks = [name, get("district"), get("city"), get("axis")]
            # Common survey-style CSVs (location_name, province, landmarks, …)
            chunks.append(raw_get_any(raw, ("landmarks", "points_of_interest", "poi")))
            chunks.append(raw_get_any(raw, ("specialties", "food", "dishes")))
            embed_text = " ".join(x for x in chunks if x)

        rows.append(
            {
                "name": name,
                "name_normalized": nn,
                "district": get("district"),
                "axis": get("axis"),
                "category": get("category"),
                "city": get("city"),
                "source": get("source") or "seed",
                "verification_status": get("verification_status") or "verified",
                "trust_score": trust,
                "embedding": embedding,
                "embed_text": embed_text,
            }
        )

    return rows, errors


def embed_model_name() -> str:
    """
    Default tag when the client omits ``embed_model`` (env chain only).
    Rows embedded via ``api_embed_text`` store the model id returned by c-lib.
    """
    from m4_default_models import OLLAMA_DEFAULT_MODEL

    e = os.environ.get("OLLAMA_EMBED_MODEL", "").strip()
    if e:
        return e
    e = os.environ.get("OLLAMA_MODEL", "").strip()
    if e:
        return e
    return OLLAMA_DEFAULT_MODEL


def ollama_host_port() -> Tuple[str, int]:
    """``OLLAMA_HOST`` / ``OLLAMA_PORT`` or compile-time defaults from c-lib (no HTTP)."""
    from engine_ctypes import c_build_ollama_default_host, c_build_ollama_default_port

    h = os.environ.get("OLLAMA_HOST", "").strip()
    p_s = os.environ.get("OLLAMA_PORT", "").strip()
    host = h if h else c_build_ollama_default_host()
    port = int(p_s) if p_s else c_build_ollama_default_port()
    return host, port


def ollama_embed(embed_text: str, embed_model: str, host: str, port: int) -> List[float]:
    """Embedding via Ollama HTTP /api/embed (ollama_embeddings removed from c-lib public API)."""
    import json
    import urllib.request
    url = f"http://{host}:{port}/api/embed"
    body = json.dumps({"model": embed_model, "input": embed_text}).encode("utf-8")
    req = urllib.request.Request(url, data=body, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as resp:
        data = json.load(resp)
    embeddings = data.get("embeddings") or data.get("embedding")
    if not embeddings:
        raise RuntimeError(f"Ollama /api/embed returned no embeddings: {data}")
    # /api/embed returns {"embeddings": [[...], ...]} — take first
    vec = embeddings[0] if isinstance(embeddings[0], list) else embeddings
    return [float(v) for v in vec]


def ollama_import_embed_endpoint() -> Dict[str, Any]:
    """
    Resolved Ollama host/port for geo CSV embed (config only). No cluster probe or model listing.
    """
    oh, op = ollama_host_port()
    return {
        "ollama_host": oh,
        "ollama_port": op,
        "note": "from OLLAMA_HOST/PORT or c-lib api_build_ollama_default_* (no HTTP probe)",
    }
