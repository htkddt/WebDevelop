# 179FC WebDevelop

179FC WebDevelop is an employee-management application with a React frontend, a NestJS public API, MongoDB, and a private FastAPI AI service for the chatbot. Each responsibility has one runtime owner: the browser never calls MongoDB, Ollama, or the AI service directly.

## Documentation

- [Frontend.md](Frontend.md): Frontend architecture, coding standards, and refactoring notes.
- [Backend.md](Backend.md): NestJS/FastAPI architecture, contracts, migration, and security.
- [Diagram.md](Diagram.md): Component, block, and sequence diagrams.
- [Yaml.md](Yaml.md): GitHub Actions YAML, CI/CD dependencies, branch policy, and troubleshooting.
- [.github/branch-protection.md](.github/branch-protection.md): Required GitHub Ruleset configuration for pull-request-only merges.
- [frontend/README.md](frontend/README.md): Frontend setup, build, test, and deployment notes.
- [frontend/src/README.md](frontend/src/README.md): React source layout and `.jsx`/`.js` conventions.
- [backend/README.md](backend/README.md): Backend operations and cross-service changes.
- [tests/README.md](tests/README.md): Shared regression-test layout and commands.

## Current architecture and request flow

```text
Browser
  └── React/Vite frontend
        ├── pages + views + layouts + shared components
        ├── config + services + hooks
        └── HashRouter / protected routes
              │ Bearer JWT
              ▼
        NestJS public API
        ├── Auth, Users, Dashboard → MongoDB
        └── Chat gateway → FastAPI private AI service → Ollama/provider
```

- Endpoint configuration, the JSON HTTP client, session storage, API modules, and reusable data hooks are separated from frontend pages.
- Login and registration are public routes; Every application route, including Settings, requires a session.
- NestJS is the only public API. The former Express mock and Flask CRUD/chat runtimes are not part of the active application flow.
- FastAPI is a private model-orchestration boundary with a health check, input validation, timeout/error handling, an internal service token, and tests that do not require Ollama.
- Regression tests are centralized in `tests/`: React/Jest tests, NestJS/Jest tests, and FastAPI/pytest tests use mocks rather than live infrastructure.
- Reusable GitHub Actions workflows build and test all three layers. Production deploys only after a push to protected `main` or `master` (normally the result of a merged pull request).

The frontend chat payload remains `contents`. NestJS adapts it to FastAPI and returns `header`, `reply`, and `conversationId`, so the browser contract remains stable.

## Repository layout

```text
.
├── frontend/
│   ├── README.md             # Frontend setup, test, build, deployment
│   └── src/
│       ├── components/ config/ hooks/ layouts/
│       ├── pages/ services/ styles/ views/
│       ├── README.md         # React and JavaScript conventions
│       └── AppRoutes.jsx
├── backend/
│   ├── README.md             # Two-service backend operations
│   ├── nestjs_ai/            # Public API: JWT, MongoDB, chat gateway
│   └── ai_service/           # Private FastAPI chatbot/model service
├── tests/
│   ├── frontend/             # React component, route, hook, and client tests
│   ├── nestjs/               # NestJS service and controller tests
│   └── ai_service/           # FastAPI route/provider tests
├── .github/
│   ├── workflows/            # CI, preview, production, reusable workflows
│   └── branch-protection.md
├── Frontend.md
├── Backend.md
├── Diagram.md
└── Yaml.md
```

## Prerequisites

- Node.js 20 (The CI standard; Test Vite 2 before any future Vite upgrade).
- Python 3.11 or newer.
- MongoDB, locally or through Atlas.
- Ollama or another provider reachable by FastAPI when chatbot responses are needed.

## Local development

Start the dependencies in this order: MongoDB, FastAPI (and Ollama when using chat), NestJS, then the frontend.

### 1. FastAPI AI service

```powershell
cd backend/ai_service
copy .env.example .env
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
uvicorn app.main:app --reload --port 8000
```

An empty `AI_SERVICE_TOKEN` is acceptable only for local development. Use a non-empty secret outside local development.

### 2. NestJS API

```powershell
cd backend/nestjs_ai
copy .env.example .env
npm ci
npm run start:dev
```

Set these values in `.env`:

```dotenv
MONGO_URL=mongodb://localhost:27017/179FC
JWT_SECRET=<long-random-secret>
FRONTEND_ORIGIN=http://localhost:3000
AI_SERVICE_URL=http://localhost:8000
AI_SERVICE_TOKEN=<same-private-token-as-ai-service>
```

NestJS listens on `http://localhost:5000/api` by default.

### 3. Frontend

```powershell
cd frontend
npm ci
npm run dev
```

Create `frontend/.env.local` when the API host differs:

```dotenv
VITE_API_BASE_URL=http://localhost:5000/api
```

Because Vite is configured with `base: '/WebDevelop/'`, use `http://localhost:3000/WebDevelop/index.html#/login` for the local dev server.

## Public API and configuration

The browser calls only NestJS endpoints under `VITE_API_BASE_URL` (locally, `http://localhost:5000/api`). FastAPI is a private implementation detail.

| NestJS endpoint | Purpose | Authentication |
| --- | --- | --- |
| `POST /api/auth/register` | Create a user with `permissions`. | None |
| `POST /api/auth/login` | Sign in and return a JWT plus the user. | None |
| `GET /api/dashboard` | Return dashboard statistics. | Bearer JWT |
| `GET /api/users/employees` | Return employees without passwords. | Bearer JWT |
| `POST /api/chat` | Send `contents` through the FastAPI chat gateway. | Bearer JWT |

| Setting | Location | Purpose |
| --- | --- | --- |
| `VITE_API_BASE_URL` | `frontend/.env.local`; repository variable in production | Public NestJS URL ending in `/api`. |
| `MONGO_URL`, `JWT_SECRET`, `FRONTEND_ORIGIN` | `backend/nestjs_ai/.env` or hosting secrets | Database, JWT, and public-API CORS configuration. |
| `AI_SERVICE_URL`, `AI_SERVICE_TOKEN` | Both backend services | Private address and NestJS-to-FastAPI authentication token. |
| `PREVIEW_DEPLOY_HOOK`, `BACKEND_DEPLOY_HOOK` | Optional GitHub repository secrets | Trigger a configured preview or backend host after a valid pipeline. |

Never commit `.env` files, database URIs, passwords, JWT secrets, or service tokens.

## Verification

Run the complete regression suite from the repository root after installing the relevant dependencies:

```powershell
npm --prefix frontend test
npm --prefix backend/nestjs_ai test
Push-Location backend/ai_service; .\.venv\Scripts\python.exe -m pytest ../../tests/ai_service -q; Pop-Location
```

Build the deployable applications as well:

```powershell
npm --prefix frontend run build
npm --prefix backend/nestjs_ai run build
```

Tests are isolated and do not require MongoDB, Ollama, service credentials, or a running browser.

## CI/CD and merge policy

- Every feature-branch push runs frontend build/tests, NestJS build/tests, FastAPI tests, and produces a frontend preview artifact.
- `PREVIEW_DEPLOY_HOOK`, when configured, can publish a branch preview without overwriting GitHub Pages.
- Pull requests into `main` or `master` run the same checks.
- A push to `main` or `master` validates all layers again, deploys the frontend to GitHub Pages, and can trigger backend deployment through `BACKEND_DEPLOY_HOOK`.
- YAML cannot prevent direct pushes by itself. Apply the Ruleset described in [.github/branch-protection.md](.github/branch-protection.md) before relying on the policy.

See [Yaml.md](Yaml.md) for workflow composition, dependencies, and troubleshooting.

## Security and deployment order

1. Rotate any credential that appeared in an old seed script; use environment variables or a secret manager going forward.
2. Flask PBKDF2 hashes cannot be verified by NestJS bcrypt. Force a password reset or implement a time-limited migration before cutover.
3. Flask JWTs become invalid once NestJS is activated; require users to sign in again.
4. Keep FastAPI private and protect NestJS-to-AI calls with a service token distinct from `JWT_SECRET`.
5. Restrict CORS with `FRONTEND_ORIGIN`, add production rate limits for login/chat, and do not log passwords or raw secrets.
6. Provision MongoDB, secrets, and private networking; deploy FastAPI and verify `/healthz`; deploy NestJS; then set `VITE_API_BASE_URL` and deploy the frontend.

For migration detail and the chatbot roadmap, see [Backend.md](Backend.md).
