# Backend Architecture and Refactor

## Current outcome

The project has one public API and one private AI service:

```text
React frontend
    │ HTTPS + Bearer JWT
    ▼
NestJS API (`backend/nestjs_ai`)
    ├── MongoDB: authentication, users, dashboard
    └── private HTTP + service token
            ▼
       FastAPI AI service (`backend/ai_service`)
            ▼
          Ollama / future model provider
```

The former Express mock (`backend/nodejs_ai`) and Flask implementation (`backend/python_ai`) are not part of the active runtime. NestJS is the only endpoint the browser may call. FastAPI is an internal service: it has no browser CORS, user authentication, or MongoDB CRUD responsibility.

See [backend/README.md](backend/README.md) for cross-service operations, [backend/nestjs_ai/README.md](backend/nestjs_ai/README.md) for the public API, and [backend/ai_service/README.md](backend/ai_service/README.md) for the AI service.

## NestJS API changes

| Area | Current design |
| --- | --- |
| Runtime boundary | `ChatService` calls FastAPI through `AI_SERVICE_URL`; it does not call Ollama directly. |
| Internal authentication | NestJS sends `X-Internal-Service-Token`; FastAPI verifies it when `AI_SERVICE_TOKEN` is configured. |
| DTO validation | `ChatRequestDto` accepts `contents`, optional `conversationId`, and a maximum of 8,000 characters. |
| JWT and configuration | `JWT_SECRET`, `MONGO_URL`, `AI_SERVICE_URL`, and `AI_SERVICE_TOKEN` are required and fail fast when absent. |
| CORS | Only the configured `FRONTEND_ORIGIN` is allowed. |
| User contract | The normalized field is `permissions`; login returns a string `id` and `permissions`. |
| Chat contract | The browser still posts `contents` to `/api/chat`; NestJS returns `header`, `reply`, and `conversationId`. |
| Tests | `npm test` runs isolated Jest tests from `tests/nestjs/` with mocked service boundaries. |

Copy `backend/nestjs_ai/.env.example` to `.env` and supply real values:

```dotenv
PORT=5000
MONGO_URL=mongodb://localhost:27017/179FC
JWT_SECRET=<long-random-secret>
FRONTEND_ORIGIN=http://localhost:3000
AI_SERVICE_URL=http://localhost:8000
AI_SERVICE_TOKEN=<same-value-as-ai-service>
```

## FastAPI AI-service boundary

`backend/ai_service` isolates chatbot/model work from web CRUD:

| Component | Responsibility |
| --- | --- |
| `app/main.py` | FastAPI application, `GET /healthz`, private `POST /v1/chat/completions`. |
| `app/schemas.py` | Pydantic request/response schemas and input limits. |
| `app/service.py` | Application boundary for future RAG, conversation memory, provider routing, or queues. |
| `app/providers/ollama.py` | Async Ollama adapter, timeout handling, and provider-error boundary. |
| `tests/ai_service/test_main.py` | Isolated route, authentication, validation, and orchestration tests. |

Run it locally:

```powershell
cd backend/ai_service
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
uvicorn app.main:app --reload --port 8000
.\.venv\Scripts\python.exe -m pytest ../../tests/ai_service -q
```

An empty `AI_SERVICE_TOKEN` is for local development only. In staging and production, FastAPI must run on a private network and use a secret that differs from the JWT secret.

## Cutover and compatibility work

1. **Rotate credentials.** A legacy Flask seed script contained a MongoDB credential; rotate it and do not restore the script or secret.
2. **Migrate passwords.** Flask used `pbkdf2_sha256` and NestJS uses `bcrypt`; legacy hashes cannot be verified by bcrypt. Force password resets or use a time-limited migration strategy.
3. **Review MongoDB documents.** Existing `permissions` documents fit the normalized model; verify the unique email index and backfill required fields before deployment.
4. **Cut over JWTs.** Flask JWTs are invalid in NestJS, so users must sign in again.
5. **Configure the frontend.** Set `VITE_API_BASE_URL` to the public HTTPS NestJS `/api` URL. Until it is set, the production workflow runs checks but skips the GitHub Pages deployment.
6. **Deploy in order.** Deploy FastAPI, verify `/healthz` from the NestJS network, deploy NestJS with the matching service token, and then switch the frontend URL. Do not expose FastAPI publicly.

## Chatbot roadmap

The FastAPI boundary is ready for future RAG/embeddings, document ingestion, model evaluation, long-running workers, and an independently operated AI team. Add capabilities in this order:

1. Persist conversation metadata/history by `conversation_id` with a retention policy.
2. Add provider abstraction, a hosted-model/Ollama fallback, SSE streaming, and circuit breaking.
3. Add RAG ingestion workers, a vector store, NestJS-provided document authorization, and evaluation datasets.
4. Add per-user quotas/rate limits in NestJS, audit events, prompt/output limits, observability, and correlation IDs.
5. Use a queue for long document processing or generation rather than holding an HTTP worker open.

## Current API contract

| Public NestJS endpoint | Input | Main output | Authentication |
| --- | --- | --- | --- |
| `POST /api/auth/register` | User plus `permissions` | Success message | Public |
| `POST /api/auth/login` | Email and password | Token plus user | Public |
| `GET /api/users/employees` | — | Employee list without passwords | Bearer JWT |
| `GET /api/dashboard` | — | `departments`, `total`, `active`, `inactive` | Bearer JWT |
| `POST /api/chat` | `contents`, optional `conversationId` | `reply`, `conversationId` | Bearer JWT |

The internal FastAPI endpoint is `POST /v1/chat/completions`, accepting `{ message, conversation_id?, user_id? }`. It is not a public contract and can evolve with the NestJS adapter.

## Verification

```powershell
# NestJS
cd backend/nestjs_ai
npm ci
npm run build
npm test

# FastAPI
cd ../ai_service
pip install -r requirements.txt
.\.venv\Scripts\python.exe -m pytest ../../tests/ai_service -q
```

CI runs equivalent commands through reusable workflows. See [Yaml.md](Yaml.md) for workflow dependencies and branch policy, and [.github/branch-protection.md](.github/branch-protection.md) for pull-request-only merge enforcement.
