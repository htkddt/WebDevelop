# 179FC WebDevelop

Ứng dụng quản lý nhân sự gồm React frontend, NestJS public API, MongoDB và FastAPI private AI service cho chatbot. Mỗi trách nhiệm chỉ có một owner runtime; browser không gọi database, Ollama hoặc AI service trực tiếp.

## Tài liệu

- [Frontend.md](Frontend.md): thống kê, coding rules và refactor frontend.
- [Backend.md](Backend.md): architecture NestJS/FastAPI, contract, migration và security.
- [Diagram.md](Diagram.md): component, block và sequence diagram.
- [Yaml.md](Yaml.md): GitHub Actions YAML, CI/CD dependency, branch policy và troubleshooting.
- [.github/branch-protection.md](.github/branch-protection.md): cấu hình Ruleset bắt buộc để chỉ merge qua pull request.
- [frontend/README.md](frontend/README.md): vận hành, build và lưu ý của toàn frontend.
- [frontend/src/README.md](frontend/src/README.md): cấu trúc React source và quy ước `.jsx`/`.js`.
- [backend/README.md](backend/README.md): vận hành và thay đổi liên-service của backend.

## Coding design hiện tại

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

### Thay đổi design đã áp dụng

- Frontend tách endpoint config, HTTP client, auth storage, API services và hook dữ liệu chung; không lặp `fetch`/base URL/localStorage key trong page.
- Public login/register tách khỏi protected `MainLayout`; toàn bộ app area, bao gồm Settings, cần session.
- Dashboard/employees có fallback đúng schema; form/menu có semantic element và accessibility cơ bản.
- NestJS là public API duy nhất. Express mock và Flask CRUD/chat legacy đã bị loại khỏi runtime source.
- FastAPI AI service là private boundary cho model orchestration, có health check, input schema, timeout/error adapter, internal service token và test không cần Ollama.
- CI/CD dùng reusable workflow cho frontend/NestJS/AI service; production deploy chỉ sau push vào protected `main/master` (tức sau merge PR khi Ruleset được bật).

Không có feature UI mới được thêm trong refactor frontend. Backend chat vẫn giữ payload frontend `contents`, nhưng NestJS giờ gateway sang FastAPI thay vì gọi Ollama trực tiếp.

## Cấu trúc

```text
.
├── frontend/
│   ├── README.md             # Vận hành/cấu hình frontend
│   └── src/
│       ├── components/ config/ hooks/ layouts/
│       ├── pages/ services/ styles/ views/
│       ├── README.md         # Quy ước React và JavaScript source
│       └── AppRoutes.jsx
├── backend/
│   ├── README.md             # Tổng quan vận hành hai backend service
│   ├── nestjs_ai/            # Public API: JWT, MongoDB, chat gateway
│   ├── ai_service/           # Private FastAPI chatbot/model service
│   └── nodejs_ai/
│       └── Document.pdf      # Tài liệu tham khảo đã khôi phục; không thuộc runtime/code flow
├── .github/
│   ├── workflows/            # CI, preview, production, reusable workflow
│   └── branch-protection.md
├── Frontend.md
├── Backend.md
├── Diagram.md
└── Yaml.md
```

## Prerequisites

- Node.js 20 (CI standard; frontend Vite 2 should be upgrade-tested before a future Vite upgrade).
- Python 3.11.
- MongoDB local/Atlas.
- Ollama or another provider reachable from FastAPI if chatbot responses are required.

## Local development

### 1. FastAPI AI service

```powershell
cd backend/ai_service
copy .env.example .env
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
uvicorn app.main:app --reload --port 8000
```

For local-only development, an empty `AI_SERVICE_TOKEN` is permitted. Set it to a non-empty secret outside local development.

### 2. NestJS API

```powershell
cd backend/nestjs_ai
copy .env.example .env
npm ci
npm run start:dev
```

Set values in `.env`:

```dotenv
MONGO_URL=mongodb://localhost:27017/179FC
JWT_SECRET=<long-random-secret>
FRONTEND_ORIGIN=http://localhost:3000
AI_SERVICE_URL=http://localhost:8000
AI_SERVICE_TOKEN=<same-private-token-as-ai-service>
```

NestJS listens at `http://localhost:5000/api` by default.

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

Open `http://localhost:3000/#/login`.

## API public và cấu hình

Browser chỉ gọi các endpoint NestJS dưới `VITE_API_BASE_URL` (mặc định local là `http://localhost:5000/api`). Endpoint FastAPI là private implementation detail, không gọi từ browser.

| Endpoint NestJS | Mục đích | Xác thực |
| --- | --- | --- |
| `POST /api/auth/register` | Tạo người dùng với `permissions`. | Không |
| `POST /api/auth/login` | Đăng nhập, trả JWT và user. | Không |
| `GET /api/dashboard` | Số liệu dashboard. | Bearer JWT |
| `GET /api/users/employees` | Danh sách nhân sự không gồm password. | Bearer JWT |
| `POST /api/chat` | Chat gateway tới FastAPI, nhận `contents`. | Bearer JWT |

| Cấu hình | Nơi đặt | Ý nghĩa |
| --- | --- | --- |
| `VITE_API_BASE_URL` | `frontend/.env.local`; repository variable khi production | Public NestJS URL, kết thúc bằng `/api`. |
| `MONGO_URL`, `JWT_SECRET`, `FRONTEND_ORIGIN` | `backend/nestjs_ai/.env` hoặc hosting secrets | Database, JWT và CORS của public API. |
| `AI_SERVICE_URL`, `AI_SERVICE_TOKEN` | Cả hai backend service | Địa chỉ private và token xác thực NestJS → FastAPI. |
| `PREVIEW_DEPLOY_HOOK`, `BACKEND_DEPLOY_HOOK` | GitHub repository secrets (tùy chọn) | Kích hoạt host preview/backend sau pipeline hợp lệ. |

Không commit `.env`, database URI, password, JWT secret hoặc service token. Mẫu biến môi trường ở README của từng backend service.

## Verification

```powershell
# frontend
cd frontend
npm run build

# NestJS API
cd ../backend/nestjs_ai
npm run build

# AI service
cd ../ai_service
pytest
```

## CI/CD and merge policy

- Every feature/sub-branch push runs CI and creates a frontend preview artifact.
- A configured `PREVIEW_DEPLOY_HOOK` can publish branch previews to a chosen host without overwriting GitHub Pages.
- Pull requests into `main/master` run the same three checks: `frontend-build`, `nest-build`, `ai-test`.
- After PR merge, `deploy-production.yml` requires repository variable `VITE_API_BASE_URL` (the public NestJS `/api` URL), runs all checks again, deploys frontend to GitHub Pages and optionally triggers backend hosting through `BACKEND_DEPLOY_HOOK`.
- Direct-push prevention is a GitHub repository Ruleset/branch-protection setting, not YAML alone. Apply the settings in `.github/branch-protection.md` before relying on this policy.

See [Yaml.md](Yaml.md) for syntax, job dependencies, workflow composition and common failures.

## Security and cutover requirements

1. Rotate any credential previously committed in a legacy seed script; use only secret manager/environment variables thereafter.
2. Existing Flask PBKDF2 password hashes do not work with NestJS bcrypt. Force a password reset or implement a time-limited legacy migration strategy before cutover.
3. Existing Flask JWTs become invalid when NestJS is activated; require login again.
4. Do not expose FastAPI AI service publicly. Keep it on a private network and protect NestJS-to-AI calls with a distinct service token.
5. Restrict CORS (`FRONTEND_ORIGIN`), add production rate limits for login/chat and do not log passwords or raw secrets.

## Deployment order

1. Provision MongoDB, secrets and private networking.
2. Deploy FastAPI AI service; verify `GET /healthz` from NestJS network.
3. Deploy NestJS with `AI_SERVICE_URL` and matching `AI_SERVICE_TOKEN`.
4. Update `VITE_API_BASE_URL`, build/deploy frontend and verify login/dashboard/chat.
5. Enable GitHub Ruleset required checks and deploy hooks/host Git integration.

Detailed backend migration and chatbot roadmap are maintained in [Backend.md](Backend.md).
