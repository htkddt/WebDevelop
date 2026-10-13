# 179FC AI Service

FastAPI service dedicated to model orchestration. It is private: browsers call NestJS, then NestJS calls this service with `X-Internal-Service-Token`.

## Local run

```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
uvicorn app.main:app --reload --port 8000
```

Health check: `GET /healthz`.

Chat contract: `POST /v1/chat/completions` with `{ "message": "...", "conversation_id": "optional" }`.

Set `AI_SERVICE_TOKEN` in non-local environments. The service deliberately has no user auth, MongoDB CRUD, browser CORS, or public deployment contract; those responsibilities belong to NestJS.
