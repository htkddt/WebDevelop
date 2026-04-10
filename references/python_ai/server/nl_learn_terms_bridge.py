# -*- coding: utf-8 -*-
"""
Helpers for **previewing** NL routing cues and default JSON path. **Recording** runs inside c-lib on each
successful chat turn when ``api_options_t.learning_terms_path`` is set and ``enable_learning_terms`` is on
(see ``src/nl_learn_cues.c`` — tables must match this module).

**Data file:** JSON v2 (``nl_learn_terms.json``) — ``{ "schema": "nl_learn_terms_v2", "terms": { "<phrase>": { "map": { "ELK_ANALYTICS": N, ... } } } }``.
Optional per-term ``meta`` is **ignored on load** and **not written back** on save (the engine only persists counts).

**Read priors from the host:** ``api_nl_learn_terms_score_sum`` (ctypes in ``engine_ctypes``).

**Fallback env (c-lib):** ``M4_NL_LEARN_RECORD_FALLBACK=1`` → ``chat`` / ``CHAT`` when no cue matched.
"""
from __future__ import annotations

import os
from typing import List, Optional, Tuple

_NL_LEARN_DATA = "nl_learn_terms.json"
_ENV_RELAX_INTENT = "M4_NL_LEARN_RELAX_INTENT"

# elk_nl_routing.md §4 / §8.1 R-L4 — must match c-lib intent_on_closed_list in nl_learn_terms.c
NL_LEARN_ALLOWED_INTENTS: frozenset[str] = frozenset(
    ("CHAT", "ELK_ANALYTICS", "ELK_SEARCH", "RAG_VECTOR")
)


def nl_learn_intent_allowed(intent: str) -> bool:
    """True if ``intent`` may be stored (closed list), or if ``M4_NL_LEARN_RELAX_INTENT`` is truthy."""
    if not intent or not intent.strip():
        return False
    s = intent.strip()
    if _relaxed_intent_list():
        return bool(s) and all(c.isalnum() or c == "_" for c in s)
    return s in NL_LEARN_ALLOWED_INTENTS


def _relaxed_intent_list() -> bool:
    v = (os.environ.get(_ENV_RELAX_INTENT) or "").strip().lower()
    return v in ("1", "true", "yes", "on")


def default_nl_learn_terms_data_path() -> str:
    """Absolute path to ``server/data/nl_learn_terms.json`` (create parent + empty v2 doc if missing)."""
    server_dir = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(server_dir, "data", _NL_LEARN_DATA)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if not os.path.isfile(path):
        with open(path, "w", encoding="utf-8") as f:
            f.write(
                "{\n"
                '  "schema": "nl_learn_terms_v2",\n'
                '  "terms": {}\n'
                "}\n"
            )
    return os.path.abspath(path)


def default_nl_learn_terms_tsv_path() -> str:
    """Deprecated alias — use ``default_nl_learn_terms_data_path`` (JSON v2)."""
    return default_nl_learn_terms_data_path()


def _normalize_utterance(utterance: str) -> str:
    return " ".join((utterance or "").lower().split())


def _best_match_in_table(u: str, table: Tuple[Tuple[str, str], ...]) -> Optional[Tuple[str, str]]:
    """Longest phrase match wins within this table."""
    best: Optional[Tuple[int, str, str]] = None
    for phrase, intent in table:
        if phrase in u:
            t = (len(phrase), phrase, intent)
            if best is None or t[0] > best[0]:
                best = t
    if not best:
        return None
    return best[1], best[2]


# --- Tier 1: analytics / aggregates (prefer over time-only when both apply) ---
_CUE_QUANT: Tuple[Tuple[str, str], ...] = (
    ("number of", "ELK_ANALYTICS"),
    ("how many", "ELK_ANALYTICS"),
    ("how much", "ELK_ANALYTICS"),
    ("total sales", "ELK_ANALYTICS"),
    ("count of", "ELK_ANALYTICS"),
    ("in total", "ELK_ANALYTICS"),
    ("per month", "ELK_ANALYTICS"),
    ("per day", "ELK_ANALYTICS"),
)

# --- Tier 2: keyword / listing → ELK_SEARCH (broad query + filters in the executor) ---
_CUE_SEARCH: Tuple[Tuple[str, str], ...] = (
    ("where can i find", "ELK_SEARCH"),
    ("do you have any", "ELK_SEARCH"),
    ("search for", "ELK_SEARCH"),
    ("look for", "ELK_SEARCH"),
    ("show me all", "ELK_SEARCH"),
    ("show me the", "ELK_SEARCH"),
    ("list all", "ELK_SEARCH"),
    ("list of", "ELK_SEARCH"),
    ("find me", "ELK_SEARCH"),
    ("find the", "ELK_SEARCH"),
    ("find a", "ELK_SEARCH"),
    ("show me", "ELK_SEARCH"),
    ("any products", "ELK_SEARCH"),
    ("in stock", "ELK_SEARCH"),
    ("on sale", "ELK_SEARCH"),
    ("browse", "ELK_SEARCH"),
    ("catalog", "ELK_SEARCH"),
    ("list", "ELK_SEARCH"),
    ("find", "ELK_SEARCH"),
)

# --- Tier 3: time windows (often ANALYTICS; also narrows SEARCH when combined in the router) ---
_CUE_TIME: Tuple[Tuple[str, str], ...] = (
    ("year to date", "ELK_ANALYTICS"),
    ("last quarter", "ELK_ANALYTICS"),
    ("this year", "ELK_ANALYTICS"),
    ("this month", "ELK_ANALYTICS"),
    ("last year", "ELK_ANALYTICS"),
    ("last month", "ELK_ANALYTICS"),
    ("last week", "ELK_ANALYTICS"),
    ("yesterday", "ELK_ANALYTICS"),
    ("today", "ELK_ANALYTICS"),
    ("ytd", "ELK_ANALYTICS"),
)

# --- Tier 4: documentation / policy → RAG_VECTOR when that path exists ---
_CUE_RAG: Tuple[Tuple[str, str], ...] = (
    ("according to documentation", "RAG_VECTOR"),
    ("from the documentation", "RAG_VECTOR"),
    ("knowledge base", "RAG_VECTOR"),
    ("in the manual", "RAG_VECTOR"),
    ("our policy", "RAG_VECTOR"),
    ("what does the doc", "RAG_VECTOR"),
    ("from the docs", "RAG_VECTOR"),
)

# --- Tier 5: extra analytic phrasing ---
_CUE_ANALYTICS_EXTRA: Tuple[Tuple[str, str], ...] = (
    ("year over year", "ELK_ANALYTICS"),
    ("month over month", "ELK_ANALYTICS"),
    ("growth rate", "ELK_ANALYTICS"),
    ("breakdown by", "ELK_ANALYTICS"),
    ("compared to", "ELK_ANALYTICS"),
    ("percentage of", "ELK_ANALYTICS"),
    ("top ten", "ELK_ANALYTICS"),
    ("top 10", "ELK_ANALYTICS"),
    ("ranking", "ELK_ANALYTICS"),
    ("average ", "ELK_ANALYTICS"),
    ("sum of", "ELK_ANALYTICS"),
    ("median", "ELK_ANALYTICS"),
)

_CUE_TIER_ORDER: Tuple[Tuple[Tuple[str, str], ...], ...] = (
    _CUE_QUANT,
    _CUE_SEARCH,
    _CUE_TIME,
    _CUE_RAG,
    _CUE_ANALYTICS_EXTRA,
)


def nl_learn_all_cue_pairs(utterance: str) -> List[Tuple[str, str]]:
    """
    All cues to record this turn: at most one (phrase, intent) per tier, deduped by phrase.
    Phrases must stay valid ``nl_learn_terms`` term strings (no TAB/NL).
    """
    u = _normalize_utterance(utterance)
    if not u:
        return []
    seen: set[str] = set()
    out: List[Tuple[str, str]] = []
    for table in _CUE_TIER_ORDER:
        m = _best_match_in_table(u, table)
        if not m:
            continue
        phrase, intent = m
        if phrase in seen:
            continue
        seen.add(phrase)
        out.append((phrase, intent))
    return out


def nl_learn_cues_from_utterance(utterance: str) -> Optional[Tuple[List[str], str]]:
    """
    **Backward compatible:** first tier that produced a match (same order as ``_CUE_TIER_ORDER``),
    longest phrase in that tier only. Prefer ``nl_learn_all_cue_pairs`` for multi-cue learning.
    """
    pairs = nl_learn_all_cue_pairs(utterance)
    if not pairs:
        return None
    phrase, intent = pairs[0]
    return ([phrase], intent)


def user_utterance_for_nl_learn(enriched_message: str) -> str:
    """
    Strip app-layer profile prefix (``... ---\\n\\n``) for display/tests. c-lib applies the same rule when recording cues.
    """
    s = (enriched_message or "").strip()
    s = s.replace("\r\n", "\n").replace("\r", "\n")
    if "\n---\n" in s:
        s = s.rsplit("\n---\n", 1)[-1].strip()
    return s
