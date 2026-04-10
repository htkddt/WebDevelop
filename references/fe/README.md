# M4 Chat — Vite frontend

Runs on **http://127.0.0.1:8000**. Vite proxies `/api/*` to Flask (default **http://127.0.0.1:5000**). If Flask prints that it bound to **5001**, set the same port for the proxy (see env table below).

**“Socket hang up” on `/api/geo/import` (browser or Python `requests` to :8000):** the dev proxy’s default timeout is short; this repo sets a long **`proxyTimeout`** for `/api`. If it still drops, call Flask **directly** (`http://127.0.0.1:5000` or your real port) — **unittest** and **`requests.post('http://127.0.0.1:5000/api/...')`** do not use Vite; **`requests.post('http://127.0.0.1:8000/api/...')`** still goes through Vite and can fail the same way as the browser.

```bash
npm install
npm run dev
```

Environment:

| Variable | Default | Meaning |
|----------|---------|---------|
| `VITE_API_URL` | (dev: empty → same-origin `/api` via proxy) | If set, browser talks to this host directly (bypasses Vite proxy). |
| `M4ENGINE_SERVER_PORT` / `VITE_FLASK_PROXY_PORT` | `5000` | **vite.config.js** proxy target port — must match Flask. |
| `VITE_PROXY_TIMEOUT_MS` | `86400000` | Proxy wait time (ms) for slow routes (e.g. geo CSV + `embed=1`). |
| `VITE_TENANT_ID` | `default` | Fallback tenant string when the server allows anonymous chat (`M4_CHAT_REQUIRE_AUTH=0`). |
| `VITE_CHAT_TENANT_ID` | → `VITE_TENANT_ID` | Used for **geo import** `tenant_id` when not logged in; ignored by chat/history when the server uses JWT identity (default). |
| `VITE_CHAT_USER_ID` | → `VITE_CHAT_TENANT_ID` | Only when **`M4_CHAT_REQUIRE_AUTH=0`**: `user` field on chat stream. |
| `VITE_CHAT_MESSAGE_PLACEHOLDER` | `Message the assistant…` | Placeholder + aria for the full-page bot composer (`/bot`). |
| `VITE_ASSISTANT_ENABLED` | `1` | Set to `0`, `false`, `no`, or `off` to hide the floating assistant dock entirely. |
| `VITE_ASSISTANT_HIDE_WHEN_PATH` | `/bot` | Hide the floating dock on this pathname (normalized). Set to `off` or `none` to show it on every route including `/bot`. |
| `VITE_ASSISTANT_TITLE` | `Assistant` | Panel heading text. |
| `VITE_ASSISTANT_INPUT_PLACEHOLDER` | → same as `VITE_CHAT_MESSAGE_PLACEHOLDER` | Floating panel textarea placeholder. |
| `VITE_ASSISTANT_FAB_LABEL` | `Open assistant` | FAB `title` and `aria-label`. |
| `VITE_ASSISTANT_CLOSE_LABEL` | `Close assistant` | Close button `aria-label`. |
| `VITE_ASSISTANT_DOCK_ARIA_LABEL` | `Chat assistant` | Dock container `aria-label`. |
| `VITE_ASSISTANT_LOG_ARIA_LABEL` | `Assistant conversation` | Log region `aria-label`. |
| `VITE_ASSISTANT_INPUT_ARIA_LABEL` | `Message to assistant` | Panel input `aria-label`. |
| `VITE_CHAT_STREAMING_SOURCE_LABEL` | `ollama` | Small badge label on the in-progress assistant row (UI only until history refresh). |
| `VITE_GEO_IMPORT_KEY` | (empty) | Sent as `X-Geo-Import-Key` when set; must match server `M4ENGINE_GEO_IMPORT_KEY` if the server requires it. |

**Server (Flask) for chat Phase 1**

| Variable | Default | Meaning |
|----------|---------|---------|
| `M4_CHAT_REQUIRE_AUTH` | `1` | Require JWT for `/api/history`, `/api/chat`, `/api/chat/stream`; identity = `user_id` from token. Set `0` for legacy anonymous `tenant_id` in body/query (tests use this). |

**Not configured in the frontend** (server / ops only): Ollama URL and model (`OLLAMA_*`, c-lib), Mongo/Redis/ELK (`M4ENGINE_*`), JWT secrets, and persisted bot option overrides (`GET/PUT /api/admin/bot-c-lib/settings` — schema from `GET /api/admin/bot-c-lib/schema`, no duplicate key list in the UI code).

**Phase 2 (planned):** anonymous chat + temp UUID in `localStorage` — not implemented; see `docs/AUTH_JWT.md`.

```bash
VITE_API_URL=http://127.0.0.1:5000 npm run dev
# optional real tenant later:
# VITE_TENANT_ID=myorg npm run dev
# optional: different stream user id from tenant
# VITE_CHAT_USER_ID=web-guest-1 npm run dev
```
