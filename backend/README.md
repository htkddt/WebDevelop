# Backend

Backend chỉ có hai runtime service. Mỗi service có README riêng; file này là điểm vào cho vận hành, thay đổi liên-service và lưu ý toàn hệ thống.

| Service | Vai trò | Bề mặt mạng | Tài liệu |
| --- | --- | --- | --- |
| `nestjs_ai/` | Public API/BFF: JWT, users, dashboard, chat gateway và MongoDB. | Public `/api` | [nestjs_ai/README.md](nestjs_ai/README.md) |
| `ai_service/` | Chatbot/model orchestration, adapter Ollama và health check. | Private, chỉ NestJS gọi | [ai_service/README.md](ai_service/README.md) |

`nodejs_ai` (Express mock) và `python_ai` (Flask legacy) đã bị xoá. Không khôi phục chúng để cùng phục vụ một production contract.

## Thứ tự chạy cục bộ

1. Khởi động MongoDB.
2. Từ `ai_service/`, copy `.env.example` thành `.env`, cài dependency và chạy FastAPI tại port `8000`.
3. Từ `nestjs_ai/`, copy `.env.example` thành `.env`, thiết lập `MONGO_URL`, `JWT_SECRET`, `AI_SERVICE_URL`, `AI_SERVICE_TOKEN`, sau đó chạy NestJS tại port `5000`.
4. Khởi động frontend; frontend chỉ gọi `http://localhost:5000/api`.

`AI_SERVICE_TOKEN` của hai service phải giống nhau ngoài local development, nhưng phải khác `JWT_SECRET`. FastAPI không được public Internet; NestJS là owner duy nhất của contract browser.

## Thay đổi và kiểm tra toàn hệ thống

- Khi đổi public endpoint hoặc DTO: cập nhật module NestJS, `frontend/src/config/api.js`, root `README.md`, `Backend.md` và kiểm tra frontend build.
- Khi đổi contract NestJS ↔ FastAPI: cập nhật adapter `ChatService`, Pydantic schema, test FastAPI và các README service liên quan.
- Trước khi merge, chạy `npm run build` trong `nestjs_ai` và `pytest` trong `ai_service`.

Architecture, API contract, migration và chatbot roadmap được duy trì tại [Backend.md](../Backend.md). CI/CD và policy merge nằm ở [Yaml.md](../Yaml.md).
