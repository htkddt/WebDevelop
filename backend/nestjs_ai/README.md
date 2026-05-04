# NestJS AI Backend

This is the NestJS backend replacement for the employee management application.
It preserves the existing Python backend features: auth, users, dashboard, and chat.

## Setup

1. Copy `.env.example` to `.env` and fill values.
2. Install dependencies:
   ```bash
   npm install
   ```
3. Start local server:
   ```bash
   npm run start:dev
   ```
4. Build production bundle:
   ```bash
   npm run build
   ```

## API Endpoints

- `POST /api/auth/login`
- `POST /api/auth/register`
- `GET /api/users/employees`
- `GET /api/dashboard`
- `POST /api/chat`

## Chat Service

This backend calls a local Ollama instance by default. Set `OLLAMA_URL` and `OLLAMA_MODEL` in `.env`.

## Notes

- JWT is used for authentication.
- MongoDB is accessed through Mongoose.
- Routes are namespaced under `/api` to match the existing frontend.
