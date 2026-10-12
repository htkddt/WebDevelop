# NestJS Public API

NestJS is the only public backend for 179FC. Its API is namespaced under `/api`; browsers do not call FastAPI or Ollama directly.

## Run and test

```powershell
copy .env.example .env
npm ci
npm run start:dev
npm test
```

Required `.env` values: `MONGO_URL`, `JWT_SECRET`, `FRONTEND_ORIGIN`, `AI_SERVICE_URL`, and `AI_SERVICE_TOKEN`. `npm test` runs the isolated Jest suite in `../../tests/nestjs`, which mocks MongoDB and the AI-service boundary.

See [Backend.md](../../Backend.md) for the API contract, Flask password-migration requirements, and cutover checklist.

## Endpoints

- `POST /api/auth/login`
- `POST /api/auth/register`
- `GET /api/users/employees` (Bearer JWT)
- `GET /api/dashboard` (Bearer JWT)
- `POST /api/chat` (Bearer JWT; gateway to the FastAPI AI service)
