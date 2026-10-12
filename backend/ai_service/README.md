# 179FC AI Service

This FastAPI service is dedicated to model orchestration. It is private: browsers call NestJS, and NestJS calls this service with `X-Internal-Service-Token`.

## Run and test locally

```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
uvicorn app.main:app --reload --port 8000
.\.venv\Scripts\python.exe -m pytest ../../tests/ai_service -q
```

Health check: `GET /healthz`.

The internal chat contract is `POST /v1/chat/completions` with `{ "message": "...", "conversation_id": "optional" }`.

Set `AI_SERVICE_TOKEN` in non-local environments. This service deliberately has no user authentication, MongoDB CRUD, browser CORS, or public deployment contract; those responsibilities belong to NestJS. Its pytest suite lives in `../../tests/ai_service` and uses mocked provider HTTP, so it does not require Ollama or a running service.
