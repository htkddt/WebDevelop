# Backend architecture and refactor

## Kết quả thực hiện

Project hiện có một public API và một private AI service:

```text
React frontend
    │ HTTPS + Bearer JWT
    ▼
NestJS API (`backend/nestjs_ai`)
    ├── MongoDB: auth, users, dashboard
    └── private HTTP + service token
            ▼
       FastAPI AI service (`backend/ai_service`)
            ▼
          Ollama / future model provider
```

Express mock (`backend/nodejs_ai`) và Flask implementation (`backend/python_ai`) đã được loại khỏi source runtime. Chúng không còn cùng phục vụ auth/users/dashboard/chat contract. NestJS là endpoint duy nhất mà browser được phép gọi; FastAPI là service nội bộ, không có CORS/browser auth hoặc MongoDB CRUD.

Cấu trúc backend hiện tại chỉ còn `backend/nestjs_ai/` và `backend/ai_service/`. Xem [backend/README.md](backend/README.md) để vận hành liên-service, [backend/nestjs_ai/README.md](backend/nestjs_ai/README.md) cho public API và [backend/ai_service/README.md](backend/ai_service/README.md) cho AI service.

## Thay đổi NestJS API

| Khu vực | Thay đổi |
| --- | --- |
| Runtime boundary | `ChatService` không gọi Ollama trực tiếp nữa; gọi FastAPI qua `AI_SERVICE_URL`. |
| Internal auth | NestJS gửi `X-Internal-Service-Token`; FastAPI kiểm tra token khi `AI_SERVICE_TOKEN` được cấu hình. |
| DTO validation | Thêm `ChatRequestDto` (`contents`, optional `conversationId`, max 8,000 ký tự); thêm dependency `class-validator` và `class-transformer`. |
| JWT/config | Bỏ JWT secret fallback cố định; `JWT_SECRET`, `MONGO_URL`, `AI_SERVICE_URL`, `AI_SERVICE_TOKEN` là required config và fail-fast khi thiếu. |
| CORS | Chỉ cho origin trong `FRONTEND_ORIGIN` thay vì allow-all. |
| User contract | Chuẩn hoá field thành `permissions` (số nhiều), login trả `id` string và `permissions`. |
| Chat contract | Frontend vẫn `POST /api/chat` với `contents`; NestJS trả `header`, `reply`, `conversationId` để không phá UI hiện tại. |

### NestJS environment

Copy `backend/nestjs_ai/.env.example` thành `.env` và điền giá trị thật:

```dotenv
PORT=5000
MONGO_URL=mongodb://localhost:27017/179FC
JWT_SECRET=<long-random-secret>
FRONTEND_ORIGIN=http://localhost:3000
AI_SERVICE_URL=http://localhost:8000
AI_SERVICE_TOKEN=<same-value-as-ai-service>
```

## FastAPI AI service scaffold

`backend/ai_service` là nền tảng để chatbot phát triển mà không lẫn với CRUD web:

| Thành phần | Vai trò |
| --- | --- |
| `app/main.py` | FastAPI app, `GET /healthz`, private `POST /v1/chat/completions`. |
| `app/schemas.py` | Pydantic request/response schema và giới hạn input. |
| `app/service.py` | Application boundary để sau này thêm RAG, conversation memory, provider routing hoặc queue. |
| `app/providers/ollama.py` | Adapter Ollama bất đồng bộ, timeout và error boundary. |
| `tests/test_main.py` | Test health và chat orchestration không cần Ollama thật. |

Chạy local:

```powershell
cd backend/ai_service
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
uvicorn app.main:app --reload --port 8000
pytest
```

Không set `AI_SERVICE_TOKEN` chỉ phù hợp local development. Ở staging/production, FastAPI phải ở private network và token này phải khác JWT secret, lưu ở secret manager.

## Migration và compatibility cần làm trước cutover

1. **Credential rotation:** Flask seed cũ có Mongo credential hard-code. Credential đó phải rotate ngay; không khôi phục script/secret này.
2. **Password migration:** Flask cũ dùng `pbkdf2_sha256`, NestJS dùng `bcrypt`. Hash cũ không thể login qua bcrypt; thực hiện forced password reset hoặc thêm strategy migration có thời hạn, rồi bỏ nó sau cutover.
3. **Mongo schema:** document cũ có `permissions`, phù hợp model NestJS sau refactor. Kiểm tra index email unique và backfill các field thiếu trước deploy.
4. **JWT cutover:** token Flask cũ không dùng được với NestJS. Buộc người dùng login lại khi chuyển API.
5. **Frontend:** đặt repository variable `VITE_API_BASE_URL` trỏ public NestJS `/api`; production workflow fail-fast khi thiếu. Frontend đã tự gửi Bearer token và Register form đã đổi field thành `permissions`.
6. **Deployment:** deploy NestJS và AI service trước, kiểm tra `/healthz`, sau đó chuyển frontend URL. Không expose FastAPI ra public internet.

## Định hướng chatbot tiếp theo

FastAPI được tách khi chatbot bắt đầu có các tín hiệu: RAG/embeddings, document ingestion, model evaluation, background worker dài hạn hoặc team AI cần lifecycle riêng. Bổ sung theo thứ tự:

1. Lưu conversation metadata/history theo `conversation_id` ở database chuyên dụng; đặt retention policy.
2. Thêm provider abstraction cho hosted model/Ollama fallback, streaming SSE và circuit breaker.
3. Thêm RAG ingestion worker, vector store, document authorization từ NestJS và evaluation dataset.
4. Thêm quota/rate limit ở NestJS theo user, audit event, prompt/output limits, observability và tracing correlation ID.
5. Dùng queue khi document processing hoặc generation kéo dài; HTTP request không nên giữ worker lâu.

## API contract hiện tại

| Public endpoint (NestJS) | Input | Output chính | Auth |
| --- | --- | --- | --- |
| `POST /api/auth/register` | user + `permissions` | success message | Public |
| `POST /api/auth/login` | email/password | token + user | Public |
| `GET /api/users/employees` | — | employee list, không password | Bearer JWT |
| `GET /api/dashboard` | — | `departments`, `total`, `active`, `inactive` | Bearer JWT |
| `POST /api/chat` | `contents`, optional `conversationId` | `reply`, `conversationId` | Bearer JWT |

FastAPI internal endpoint: `POST /v1/chat/completions` nhận `{ message, conversation_id?, user_id? }`. Endpoint này không phải public contract và có thể thay đổi cùng NestJS adapter.

## Verification

```powershell
# NestJS
cd backend/nestjs_ai
npm ci
npm run build

# FastAPI
cd ../ai_service
pip install -r requirements.txt
pytest
```

CI chạy các lệnh tương đương qua reusable workflow. Xem `Yaml.md` để biết dependency/branch policy và `.github/branch-protection.md` để bật PR-only merge.
