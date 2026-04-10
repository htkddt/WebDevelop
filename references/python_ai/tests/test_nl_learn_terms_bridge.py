# -*- coding: utf-8 -*-
from __future__ import annotations

import os
import sys
import unittest

_PYTHON_AI = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _PYTHON_AI not in sys.path:
    sys.path.insert(0, _PYTHON_AI)


class TestNlLearnTermsBridge(unittest.TestCase):
    def test_user_utterance_strips_profile_block(self):
        from server.nl_learn_terms_bridge import user_utterance_for_nl_learn

        raw = "Topic: x\n---\n\nhow many widgets?"
        self.assertEqual(user_utterance_for_nl_learn(raw), "how many widgets?")

    def test_cues_how_many(self):
        from server.nl_learn_terms_bridge import nl_learn_cues_from_utterance

        r = nl_learn_cues_from_utterance("How many computers sold this year?")
        self.assertIsNotNone(r)
        terms, intent = r
        self.assertIn("how many", terms)
        self.assertEqual(intent, "ELK_ANALYTICS")

    def test_cues_none_when_no_match(self):
        from server.nl_learn_terms_bridge import nl_learn_cues_from_utterance

        self.assertIsNone(nl_learn_cues_from_utterance("hello there"))

    def test_all_cues_quant_and_time(self):
        from server.nl_learn_terms_bridge import nl_learn_all_cue_pairs

        pairs = nl_learn_all_cue_pairs("How many computers sold this year?")
        self.assertEqual(len(pairs), 2)
        self.assertEqual(pairs[0], ("how many", "ELK_ANALYTICS"))
        self.assertEqual(pairs[1], ("this year", "ELK_ANALYTICS"))

    def test_all_cues_search_longest_in_tier(self):
        from server.nl_learn_terms_bridge import nl_learn_all_cue_pairs

        pairs = nl_learn_all_cue_pairs("Please find laptops on sale")
        self.assertEqual(len(pairs), 1)
        self.assertEqual(pairs[0], ("on sale", "ELK_SEARCH"))

    def test_all_cues_rag(self):
        from server.nl_learn_terms_bridge import nl_learn_all_cue_pairs

        pairs = nl_learn_all_cue_pairs("What does the doc say about returns?")
        self.assertTrue(any(p == ("what does the doc", "RAG_VECTOR") for p in pairs))

    def test_nl_learn_intent_allowed_closed_list(self):
        from server import nl_learn_terms_bridge as b

        self.assertTrue(b.nl_learn_intent_allowed("ELK_ANALYTICS"))
        self.assertTrue(b.nl_learn_intent_allowed("CHAT"))
        self.assertFalse(b.nl_learn_intent_allowed("KNOWLEDGE"))
        self.assertFalse(b.nl_learn_intent_allowed("CUSTOM_INTENT"))
        self.assertFalse(b.nl_learn_intent_allowed(""))

    def test_nl_learn_intent_allowed_relaxed_env(self):
        from server import nl_learn_terms_bridge as b

        os.environ["M4_NL_LEARN_RELAX_INTENT"] = "1"
        try:
            self.assertTrue(b.nl_learn_intent_allowed("CUSTOM_INTENT"))
            self.assertFalse(b.nl_learn_intent_allowed("bad-intent"))
        finally:
            os.environ.pop("M4_NL_LEARN_RELAX_INTENT", None)


if __name__ == "__main__":
    unittest.main()
