# NestJS public API

NestJS là public backend duy nhất của 179FC. API được namespace dưới `/api`; browser không gọi FastAPI/Ollama trực tiếp.

## Run

```powershell
copy .env.example .env
npm ci
npm run start:dev
```

Required `.env`: `MONGO_URL`, `JWT_SECRET`, `FRONTEND_ORIGIN`, `AI_SERVICE_URL`, `AI_SERVICE_TOKEN`. Xem [Backend.md](../../Backend.md) để biết contract, migration password từ Flask và checklist cutover.

## Endpoints

- `POST /api/auth/login`
- `POST /api/auth/register`
- `GET /api/users/employees` (Bearer JWT)
- `GET /api/dashboard` (Bearer JWT)
- `POST /api/chat` (Bearer JWT; gateway đến FastAPI AI service)
