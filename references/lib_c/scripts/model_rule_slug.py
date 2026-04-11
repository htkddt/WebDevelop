#!/usr/bin/env python3
"""Map Ollama model id → .cursor/models/{slug}.rule.md filename stem. See .cursor/models/README.md."""
import re
import sys


def model_rule_slug(model_id: str) -> str:
    s = (model_id or "").strip().lower()
    s = re.sub(r"[:/\s]+", "-", s)
    s = re.sub(r"-+", "-", s).strip("-")
    return s if s else "default"


def main() -> None:
    if len(sys.argv) < 2:
        print("usage: model_rule_slug.py <ollama_model_name>", file=sys.stderr)
        sys.exit(1)
    print(model_rule_slug(sys.argv[1]))


if __name__ == "__main__":
    main()
