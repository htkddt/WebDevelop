# Frontend

Frontend là ứng dụng React + Vite, chịu trách nhiệm hiển thị giao diện và gọi **duy nhất** NestJS public API. Frontend không kết nối trực tiếp MongoDB, FastAPI AI service hoặc Ollama.

## Cấu trúc và trách nhiệm

| Vị trí | Trách nhiệm |
| --- | --- |
| `src/` | Source React, cấu trúc và quy ước chi tiết ở [src/README.md](src/README.md). |
| `index.html` | HTML entry point của Vite. |
| `vite.config.js` | Cấu hình Vite và base path GitHub Pages. |
| `package.json` | Scripts và dependency của frontend. |
| `dist/` | Kết quả build cục bộ, không phải source để chỉnh sửa. |

## Chạy cục bộ

```powershell
cd frontend
npm ci
npm run dev
```

Mặc định ứng dụng mở tại `http://localhost:3000/#/login` và gọi `http://localhost:5000/api`.

Khi API nằm ở host khác, tạo `frontend/.env.local`:

```dotenv
VITE_API_BASE_URL=http://localhost:5000/api
```

Chỉ biến bắt đầu bằng `VITE_` mới được Vite đưa vào client bundle. Không đặt token, mật khẩu, MongoDB URI hoặc service secret trong biến `VITE_*`.

## Build và kiểm tra

```powershell
npm run build
npm run preview
```

`npm run build` là kiểm tra bắt buộc trong CI. Với production, repository variable `VITE_API_BASE_URL` phải là public URL của NestJS và kết thúc bằng `/api`.

## Lưu ý cập nhật

- Giữ `HashRouter` vì frontend đang deploy được trên GitHub Pages.
- API URL, HTTP header và session storage phải đi qua `src/config/` và `src/services/`; page/view không hard-code endpoint.
- Mọi thay đổi public API phải được cập nhật đồng thời trong `src/config/api.js`, tài liệu root `README.md` và `Backend.md`.
- Không thêm feature hoặc thay đổi contract chỉ vì chỉnh coding design.
