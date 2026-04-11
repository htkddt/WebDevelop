# PROMPT STRATEGY: LOGICAL PERSONA & TAGS (“MẮM”)

**What this file is:** **Global** behavior for the chat agent—identity, tone, anti-hallucination rules, and the **whitelist** of runtime prompt tags (`API_PROMPT_TAG_*`). It is **not** the only place that shapes what the model sees, and it is **not** a substitute for **per-model** or **per-API-family** wiring.

**What this file is not:**

| Concern | Where it lives |
|--------|----------------|
| **Compiled default system block** | `API_CTX_SYSTEM_GUARD` in `api.c` (must stay aligned with §1–3 here in spirit). |
| **Lane / intent → Ollama tag + inject text** | `model_switch` (`.cursor/model_switch.md`, `model_switch.c`). |
| **Backend wire format** (OpenAI `messages` vs Gemini `contents` vs Ollama single prompt) | Adapter layer: **`src/ai_agent.c`** + **`.cursor/models/models/ai_agent.md`**. |
| **Quirks for one model id** (refusals, language, length) | **`.cursor/models/{slug}.rule.md`** — keyed by **resolved model id**, not by guessing from substrings. |
| **Embedding / vector policy** | `embed.h` / `.cursor/rules/embed-vector-metadata.mdc` — separate from chat persona. |

**Implementation:** Persona defaults are compiled into `API_CTX_SYSTEM_GUARD` in `api.c`. **Initial overrides** via `api_options_t.default_persona` / `api_options_t.default_instructions` at `api_create`. Internal `api_set_prompt_tag()` is no longer public — tags are set at init time. See `ctx_build_prompt` and `docs/FLOW_DIGRAM.md` §8.

---

## How to classify “model type” (separate logic — don’t grep alone)

Use a **small fixed taxonomy** first; use **substring / `grep`-style** heuristics only as a **fallback** inside adapters.

1. **Provider / API family (preferred for branching)**  
   Derive from **which path handled the turn**, not from scanning arbitrary model strings. The library records this on the active session as **`API_CHAT_WIRE_*`** after each assistant reply (available via `api_stats_t.last_chat_wire`); values match the **`ai_agent.c`** tier or Ollama/Redis/external path.
   - **`openai_compatible`** — Groq, Cerebras (`chat/completions`, `messages[]`).
   - **`gemini`** — Google `generateContent` / `contents` + `parts`.
   - **`ollama`** — local `/api/generate` (and stream) as in `ollama.c`.  
   In code this is effectively: the **tier** that succeeded in `ai_agent.c`, or **Ollama** when local/pinned. That gives you a **stable enum** for “how do I pack system + history?” without `strstr(model, "llama")`.

2. **Resolved chat `model_id` (preferred for content policy)**  
   After routing: the **actual** string sent to the vendor (e.g. `llama-3.1-8b-instant`, `gemini-2.0-flash`, `llama3.2:1b`) or the **lane-resolved Ollama tag**. Use this to pick **`.cursor/models/<slug>.rule.md`** (see **`.cursor/rules/models-folder.mdc`**). This is the right place for “this model over-explains” or “use shorter system on this tier.”

3. **Substring heuristics (`strstr` / regex on model name)**  
   Optional **last resort** for legacy or unknown ids (e.g. map `qwen` → ChatML-style wrapping **if** you add that adapter). **Risks:** collisions, renamed vendor strings, and **security** (don’t branch trust boundaries on user-influenced strings). If you use it, confine it to **presentation** (how to wrap), not to **what** policy applies.

**Rule of thumb:** **`ptomp.md`** = what the product **wants every model to respect** (persona, tags, time). **`.cursor/models/*.rule.md`** = what **this model** needs extra or different. **`models/ai_agent.md`** = how to **encode** the same logical text per API family.

---

## 1. Identity Guardrails
- **Role**: "Mắm" - A gritty Saigon local, Senior C Engineer, wearing flip-flops (tông Lào).
- **Anti-AI Constraint**: Strictly FORBIDDEN to say "I am an AI", "I am a C program", or "I apologize".
- **Tone**: Concise, salty, street-smart (Saigon slang).

## 2. Scale 3 (Mixed Language) Handling
- **Logic**: If `lang == mixed`, parse multilingual entities (Thai/English/Vietnamese) but reply EXCLUSIVELY in Saigon-style Vietnamese.
- **Anti-Hallucination**: If no context is found in Memory/ELK, say "I don't know that spot" instead of inventing California/US National Park locations.

## 3. Command Execution
- **Direct Action**: If user says "Speak English", switch 100% to English immediately. Do NOT explain the transition (e.g., No "I will now speak English...").
- **Conciseness**: Avoid over-explanation. Get straight to the point.

---

# SUPPORTED PROMPT TAGS (runtime, whitelist)

Prompt tags are configured via `api_options_t.default_persona` / `api_options_t.default_instructions` at `api_create`. Supported keys:

| Tag key (`API_PROMPT_TAG_*`) | Role |
|------------------------------|------|
| `system_time` | Optional override. If **unset**, the library prepends `[SYSTEM_TIME: YYYY-MM-DD HH:MM]` from **local wall clock** (`time`/`localtime`/`strftime`) on each prompt build—**unless** `api_options_t.disable_auto_system_time` is non-zero. If set, value may be `YYYY-MM-DD HH:MM` (wrapped + instruction) **or** a full line starting with `[SYSTEM_TIME:` (passed through). |
| `persona` | Replaces the default Mắm system block when set; clear tag to restore compiled default. |
| `instructions` | Optional extra system text after persona, before chat history. |

**Compose order** (after Topic + optional `[KNOWLEDGE_BASE]`): `system_time` → `persona` (or default) → `instructions` → strip-5 history.

To add a new tag: define `API_PROMPT_TAG_*` in `api.h`, bump `API_PROMPT_TAG_SLOTS` and `prompt_tag_slot_for_key()` / render order in `api.c`.

---

# TEMPORAL FLOW: SYSTEM TIME IN THE PROMPT

## 1. Default (no host call)
- **Mechanism**: If `API_PROMPT_TAG_SYSTEM_TIME` is **not** set and `disable_auto_system_time == 0` in `api_options_t`, the library injects **current local time** each time `ctx_build_prompt` runs (same process timezone as the engine; set `TZ` if needed).
- **Why**: Without `[SYSTEM_TIME]`, the model guesses the year from training data (e.g. wrong answers for “năm nay là năm mấy?”).

## 2. Host override (optional)
- **Mechanism**: Auto from wall clock when `api_options_t.disable_auto_system_time == 0` (default). Set `disable_auto_system_time = 1` to suppress.
- **Format**: `[SYSTEM_TIME: YYYY-MM-DD HH:MM]` when value is not already prefixed with `[SYSTEM_TIME:`.
- **Instruction block**: `API_SYSTEM_TIME_INSTRUCTION` in `api.c` tells the model to use only this line for calendar-year / “năm nay” questions.

---

# Model-specific wire format (reference — not all implemented as one C helper)

Today, **one** unified text blob from `ctx_build_prompt` is sent to **Ollama** and (in **`ai_agent.c`**) as the **user** content for cloud chat APIs. **Future or parallel work:** split **system** vs **history** per API family as in **`.cursor/models/models/ai_agent.md`**.

| API family | Typical shape | Notes |
|------------|----------------|-------|
| **OpenAI-compatible** (Groq, Cerebras, many hosted Llama endpoints) | `messages[]` with optional dedicated **`system`** role | Prefer mapping persona + tags + inject → **system** message, then alternating user/assistant. |
| **Gemini** | `contents` / `parts`; system instruction per current Google docs | Not the same JSON as OpenAI; adapter must not assume `messages`. |
| **Ollama** | Single prompt string (current `ollama_query*`) | Matches today’s pipeline; stream uses same family. |

**Formatting constraints (policy, not a single function name):**
1. **No hallucination:** Where the task needs it, add explicit output constraints in **instructions** or in the **model rule** file — avoid blindly appending “JSON only” on every turn unless the product requires it.
2. **Vietnamese:** For models that follow Vietnamese system prompts better, note that in **`.cursor/models/{slug}.rule.md`** rather than forking **all** of `ptomp.md`.
3. **PII / masking:** If you add masking, implement it in a **dedicated** sanitization stage before `ctx_build_prompt` or in the adapter; do not assume a `formatter.c` unless it exists in the repo.

---

---

## Streaming (multi-tenant message tracking)

> Merged from `streaming.md`. Implementation: `api_chat(..., stream_cb, userdata)` unified in `src/api.c`.

- Every stream generates a `temp_message_id` (UUID) internally.
- Tokens are delivered via `api_stream_token_cb` from an internal pthread.
- **UTF-8 safety**: Stream worker holds incomplete UTF-8 trailing bytes and flushes them with the next chunk (avoids mojibake on multi-byte characters).
- **Concurrency**: Each stream maps a `curl_handle` to its own context. Tokens from User_A do not leak into User_B's stream.
- **Finalization**: When `done_flag == 1`, the engine triggers `geo_learning` enqueue and `engine_append_turn` to MongoDB with the `temp_message_id`.

## Cross-links

- **Adapter taxonomy & prompt mapping:** `.cursor/models/models/ai_agent.md`
- **Cloud routing & env:** `.cursor/models/ai_agent.md`
- **Per-model rules:** `.cursor/rules/models-folder.mdc` → `.cursor/models/*.rule.md`
- **Lane / inject:** `.cursor/model_switch.md`
