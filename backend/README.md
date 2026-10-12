# Backend

The backend has two runtime services. Each has its own README; this file is the starting point for operations, cross-service changes, and system-wide safeguards.

| Service | Role | Network surface | Documentation |
| --- | --- | --- | --- |
| `nestjs_ai/` | Public API/BFF for JWT, users, dashboard, chat gateway, and MongoDB. | Public `/api` | [nestjs_ai/README.md](nestjs_ai/README.md) |
| `ai_service/` | Chatbot/model orchestration, Ollama adapter, and health check. | Private; called only by NestJS | [ai_service/README.md](ai_service/README.md) |

The Express mock (`nodejs_ai`) and Flask implementation (`python_ai`) are not active runtime services. Do not restore them to serve the same production contract.

## Local startup order

1. Start MongoDB.
2. In `ai_service/`, copy `.env.example` to `.env`, install dependencies, and run FastAPI on port `8000`.
3. In `nestjs_ai/`, copy `.env.example` to `.env`, set `MONGO_URL`, `JWT_SECRET`, `AI_SERVICE_URL`, and `AI_SERVICE_TOKEN`, then run NestJS on port `5000`.
4. Start the frontend, which calls only `http://localhost:5000/api`.

`AI_SERVICE_TOKEN` must have the same value in both backend services outside local development and must differ from `JWT_SECRET`. FastAPI must not be exposed to the public internet; NestJS owns the browser contract.

## Cross-service changes and checks

- When changing a public endpoint or DTO, update the NestJS module, `frontend/src/config/api.js`, root [README.md](../README.md), [Backend.md](../Backend.md), and relevant regression tests.
- When changing the NestJS-to-FastAPI contract, update `ChatService`, the Pydantic schema, `tests/ai_service/`, and the affected service READMEs.
- Before merging, run `npm run build` and `npm test` in `nestjs_ai`, `npm run build` and `npm test` in `frontend`, and `pytest ../../tests/ai_service -q` from `ai_service`.

Architecture, API contracts, migration, and the chatbot roadmap are maintained in [Backend.md](../Backend.md). CI/CD and merge policy are in [Yaml.md](../Yaml.md).
