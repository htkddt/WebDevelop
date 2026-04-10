# Chat stream: callback contract and backends

This document describes how **`POST /api/chat/stream`** stays **agnostic of the LLM wire format** while supporting multiple providers. Implementation: **`stream_chat_backends.py`**.

## Goals

1. **One sink per request** — The HTTP layer registers where events go (today: a `queue.Queue.put` that becomes SSE `data: {json}`). The sink does not parse Ollama NDJSON or Groq SSE.
2. **Per-provider adapters** — Each backend knows one protocol (c-lib + Ollama, OpenAI-compatible SSE, …). Backends call the sink with the **same event shape**.
3. **Separate from non-stream chat** — `POST /api/chat` still uses **`cloud_llm.c`** (full body). Streaming hosted models uses **Python + stdlib HTTP** here; persistence for hosted stream completion uses **`api_chat_external_reply_with_meta`** so Mongo/session get the correct **`llm_model_id`** and **`API_CHAT_WIRE_*`**.

## Event shape (`StreamSink`)

Each event is a **`dict`** (serialized as one SSE JSON object):

| Field | Meaning |
|--------|--------|
| `token` | UTF-8 text chunk (may be empty on the final event). |
| `temp_message_id` | Correlation id from the client or c-lib. |
| `done` | `true` when the turn is finished (Ollama/c-lib always sends a final `done`; router may add an extra final row with metadata only). |
| `error` | Optional; if set, turn failed (still often with `done: true`). |
| `source` | Optional; fine-grained label when known (`GROQ`, `OLLAMA`, `REDIS_RAG`, …) — see **`completion_source_label`** in `engine_ctypes.py`. |
| `llm_model` | Optional; e.g. `groq:llama-3.1-8b-instant`, `ollama:qwen2.5`. |

The **client** should treat **`done`** as the lifecycle boundary; **`source` / `llm_model`** are optional hints for UI and logging.

## Modes (`M4_CHAT_STREAM_MODE`)

| Value | Behavior |
|--------|-----------|
| `ollama` (default) | **`api_chat_stream`** only — same as before: Redis RAG short-circuit inside C, then Ollama NDJSON stream. |
| `router` | **`api_chat_prepare_external_llm`** → Redis hit → synthetic chunks; **lane pins Ollama** → **`api_chat_stream_from_prepared`**; else **hosted pool** in **`M4_CLOUD_TRY_ORDER`** (Groq/Cerebras = real OpenAI SSE; Gemini = one **`generateContent`** then chunked tokens until true stream exists). Then **`api_chat_external_reply_with_meta`**. **Ollama fallback** only if **`M4_CHAT_BACKEND`** is default / **`cloud_then_ollama`**: **`M4_CHAT_BACKEND=ollama`** skips hosted; **`M4_CHAT_BACKEND=cloud`** returns error if all hosted tiers fail (matches **`cloud_llm.c`**). |

Unset or unknown non-router values fall back to **`ollama`** when the env is empty; invalid strings produce one SSE error event.

## c-lib symbols used by the router

- **`api_chat_prepare_external_llm`** — Push user, optional Redis RAG completion, else build prompt (same as external / cloud prep).
- **`api_chat_stream_from_prepared`** — Ollama stream **without** pushing the user again (session already updated by prepare).
- **`api_chat_external_reply_with_meta`** — Persist hosted full reply with **`groq:…` / `cerebras:…`** and **`API_CHAT_WIRE_OPENAI_CHAT`** (not the legacy `"external"` label).

## Adding a backend

1. Implement a function that accepts the same **tenant/user/message/temp_message_id** and a **`sink`**.
2. From provider callbacks, only call **`sink({...})`** using the table above.
3. Register it from **`dispatch_chat_stream`** (new `M4_CHAT_STREAM_MODE` or a branch inside **`router`**).

**Gemini** in **`router`**: uses **non-stream** **`generateContent`** then synthetic SSE chunks (same answer as **`api_chat`**, different delivery). True Gemini byte-stream can replace this later.

## Threading and GIL

Ollama paths invoke **`api_stream_token_cb`** from a **pthread** inside c-lib. Handlers must use **`gil_held_for_c_callback()`** (see **`c_pthread_bridge`**) before touching Python objects. The Flask route still runs the **`dispatch_chat_stream`** call **inside `with _ctx_lock`** on a worker thread so **`_ctx`** matches **`api_chat`**.

## Minimal client integration

**Headers:** `Content-Type: application/json`. If **`M4_CHAT_REQUIRE_AUTH=1`**, add `Authorization: Bearer <access_token>` (same token as **`POST /api/chat`**).

**Body (JSON):** at least `{"message":"Hello"}`. Optional: `"temp_message_id":"<uuid>"` (generate on the client so the UI can correlate bubbles).

**Read the body** as a byte stream (not JSON). Split on SSE record boundaries: blank line `\n\n` separates events. Each event may contain one or more lines; lines starting with `data: ` are payload — join if needed, then **`JSON.parse`** the rest (after the `data: ` prefix).

**Stop** when you see **`"done":true`** (last event may have empty `token`). If **`error`** is set, show it and still expect **`done`**.

### curl (anonymous server, auth off)

```bash
curl -N -s -X POST "http://127.0.0.1:5000/api/chat/stream" \
  -H "Content-Type: application/json" \
  -d '{"message":"ping","temp_message_id":"cli-test-1"}'
```

(`-N` disables curl buffering so lines arrive live.)

### fetch (browser or Node)

```javascript
const res = await fetch(`${apiBase}/api/chat/stream`, {
  method: "POST",
  headers: {
    "Content-Type": "application/json",
    ...(token ? { Authorization: `Bearer ${token}` } : {}),
  },
  body: JSON.stringify({
    message: text,
    temp_message_id: crypto.randomUUID(),
  }),
});
const reader = res.body.getReader();
const dec = new TextDecoder();
let buf = "";
for (;;) {
  const { value, done } = await reader.read();
  if (done) break;
  buf += dec.decode(value, { stream: true });
  let idx;
  while ((idx = buf.indexOf("\n\n")) >= 0) {
    const block = buf.slice(0, idx);
    buf = buf.slice(idx + 2);
    for (const line of block.split("\n")) {
      if (!line.startsWith("data:")) continue;
      const ev = JSON.parse(line.slice(5).trim());
      if (ev.token) appendToAssistant(ev.token);
      if (ev.error) showError(ev.error);
      if (ev.done && (ev.source || ev.llm_model)) setMeta(ev.source, ev.llm_model);
      if (ev.done) return;
    }
  }
}
```

Point **`apiBase`** at Flask (e.g. `http://127.0.0.1:5000` or **`VITE_API_URL`**). Same-origin dev often uses `""` and path `/api/chat/stream`.
