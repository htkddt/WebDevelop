---
model_id: "llama3.1:8b-instruct-q8_0"
aliases:
  - "llama3.1:8b"
  - "llama3.1:8b-instruct-q4_0"
extends: default
supports:
  structured_json: moderate
  vietnamese_colloquial: good
  long_context: "8k class (quant dependent)"
  instruction_leak_risk: medium
risk:
  - Strong “helpful refusals” on role/identity; steer system prompt toward concise self-description instead of arguing with user tone.
prompt:
  - Geo / Mongo / tech queries: keep answers on-topic; do not default to generic DB lectures when the user switched topic (e.g. “AI là gì”).
---

# llama3.1 8B instruct (q8_0 family)

Use this rule when the active Ollama tag is Llama 3.1 8B. **Compile-time chat fallback** is **`OLLAMA_DEFAULT_MODEL`** in **`include/ollama.h`** (see **`.cursor/default_models.md`**); override with **`OLLAMA_MODEL`**, **`model_switch`** lanes, or **`M4_MODEL_*`**.

**When editing** `api.c` / `python_ai` prompts or smart_topic routing, check this file for known quirks. Overlap with **`default.rule.md`** (`extends: default`).
