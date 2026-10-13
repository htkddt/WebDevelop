# React source

`src/` chứa source code giao diện. Quy ước này giữ phần React có JSX tách rõ khỏi JavaScript thuần và giới hạn hướng phụ thuộc của code.

## Bản đồ source

```text
src/
├── assets/       # asset được Vite bundle
├── components/   # UI tái sử dụng: navigation, chat, guard, wrapper
├── config/       # cấu hình môi trường và endpoint API
├── hooks/        # React hook tái sử dụng
├── layouts/      # bố cục dùng chung của vùng đã đăng nhập
├── pages/        # điều phối state, navigation và use case
├── services/     # HTTP client, API module, session storage
├── styles/       # CSS global và CSS theo khu vực/component
├── views/        # presentation component nhận props từ page
├── App.jsx       # app root
├── AppRoutes.jsx # khai báo public/protected routes
└── main.jsx      # React entry point
```

`public/` trong `src/` là asset hiện có của project; asset mới nên ưu tiên `assets/` khi cần Vite quản lý và hash URL lúc build.

## Vì sao dùng cả `.jsx` và `.js`?

Vite có thể transpile JSX trong cả hai phần mở rộng, nhưng project dùng phần mở rộng để biểu lộ mục đích:

- **`.jsx`**: module React có JSX/render UI, ví dụ `AppRoutes.jsx`, page, view, layout và component. Phần mở rộng giúp người đọc và tooling nhận ra file có markup React.
- **`.js`**: JavaScript không render JSX, ví dụ `config/api.js`, `services/apiClient.js`, `services/authStorage.js` và các helper. Các file này có thể được tái sử dụng, kiểm thử mà không phụ thuộc UI.

Không đổi toàn bộ `.js` thành `.jsx` hoặc ngược lại chỉ để đồng nhất tên file: điều đó che mất ranh giới UI và logic. Nếu một module `.js` bắt đầu trả JSX, đổi nó thành `.jsx`; nếu module `.jsx` không còn render JSX, đổi lại `.js` trong cùng thay đổi có cập nhật import.

## Quy ước phụ thuộc

```text
views/components → props và callback từ pages/layouts
pages/layouts     → hooks, services, views/components
hooks             → services
services          → config
config            → environment variables (VITE_*)
```

- `views/` chỉ trình bày UI; không gọi `fetch`, đọc `localStorage` hay hard-code URL.
- `pages/` quản lý loading/error/fallback và chuyển dữ liệu cho view.
- Chỉ `services/apiClient.js` gửi HTTP request; token được lấy từ `authStorage.js`.
- Endpoint và base URL đặt tại `config/api.js`.
- Component tương tác dùng phần tử semantic (`button`, `label`, `NavLink`) và có nhãn truy cập phù hợp.
