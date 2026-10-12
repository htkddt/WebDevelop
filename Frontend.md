# Phân tích Frontend

## Tóm tắt hiện trạng

| Hạng mục | Thống kê |
| --- | --- |
| Runtime | React 17, Vite 2.9, React Router 6.3 |
| UI dependency | `lucide-react` |
| Màn hình | 9 page và 9 view: đăng nhập, đăng ký, dashboard, employees, leave, report, profile, assets, settings |
| Thành phần khung | `MainLayout`, `Topbar`, `Leftbar`, `ChatWidget`, `ProtectedRoute` |
| Styling | CSS thuần theo từng khu vực; chưa có design token/component UI dùng chung hoàn chỉnh |
| Routing | `HashRouter`, phù hợp khi deploy GitHub Pages |
| API hiện dùng | NestJS: `http://localhost:5000/api` khi local; production bắt buộc inject `VITE_API_BASE_URL` trỏ vào NestJS `/api` |

Tài liệu vận hành frontend nằm ở [frontend/README.md](frontend/README.md); quy ước source React/JavaScript nằm ở [frontend/src/README.md](frontend/src/README.md).

## Coding rules nên áp dụng lâu dài

1. Dùng function component, tên PascalCase cho component và camelCase cho biến/hàm. Một file page/view/component chỉ export một component mặc định chính.
2. Giữ phân tầng: `views/` chỉ render UI và nhận props; `pages/` điều phối state/use case; `services/` gọi HTTP và lưu session; `config/` chứa cấu hình môi trường; `hooks/` chứa logic React tái sử dụng.
3. Không đặt URL API, `fetch`, `localStorage` key hoặc header HTTP rải rác trong page/component. Mọi request đi qua `services/apiClient.js`; endpoint ở `config/api.js`.
4. Cấu hình thay đổi theo môi trường phải dùng biến `VITE_*` (ví dụ `VITE_API_BASE_URL`), không hard-code host production trong từng component.
5. API JSON phải gửi `Content-Type: application/json`; token, khi có, phải đi trong `Authorization: Bearer <token>`. Không dùng header tự đặt tên `type` để biểu diễn JSON.
6. Component có tương tác phải dùng HTML semantic: `button` cho action, `NavLink`/`Link` cho navigation, `label htmlFor` cho form input, và có `aria-label` khi button chỉ chứa icon.
7. State bất đồng bộ cần đủ ba trạng thái: loading, success và error/fallback; không cập nhật state sau khi component đã unmount.
8. Props backend cần được xem là dữ liệu không tin cậy: dùng fallback/null guard ở UI; không gọi trực tiếp `.charAt()` trên field có thể thiếu.
9. Không nhúng style lớn inline. Ưu tiên class CSS, CSS custom properties ở global và class có prefix theo component/feature.
10. Source file phải lưu UTF-8. Mã tiếng Việt hiển thị thành `Ã`/`ðŸ` trong terminal là dấu hiệu cần kiểm tra encoding trước khi sửa nội dung.

## Những phần nên giữ

- Tách `pages/` và `views/` là nền tảng tốt cho container/presentation pattern.
- `MainLayout` với `Outlet`, `Topbar`, `Leftbar`, `ChatWidget` là khung dùng lại rõ ràng cho khu vực đã đăng nhập.
- `HashRouter` và `base: '/WebDevelop/'` phù hợp với GitHub Pages hiện tại.
- Cơ chế hiện thị fallback khi backend không kết nối là ý tưởng tốt cho demo/deploy tĩnh; cần giữ nhưng chuẩn hoá hình dạng data.
- Các stylesheet đã có prefix khu vực như `dashboard-*`, `employee-*`, `chat-*`; nên tiếp tục quy ước này thay vì selector chung chung.
- Vite + React là toolchain nhỏ gọn, phù hợp quy mô hiện tại.

## Các thay đổi coding design đã thực hiện

| Vị trí | Thay đổi | Lý do |
| --- | --- | --- |
| `src/config/api.js` | Tập trung base URL và toàn bộ endpoint; hỗ trợ `VITE_API_BASE_URL` | Không lặp URL local/production ở bốn component |
| `src/services/apiClient.js` | Một HTTP client JSON, chuẩn hoá lỗi và tự gắn Bearer token | Sửa header JSON sai và chuẩn bị tương thích NestJS guard |
| `src/services/api.js` | Tách API auth/dashboard/employees/chat | Page chỉ còn điều phối use case |
| `src/services/authStorage.js` | Tập trung key và thao tác session | Tránh magic string `token`/`user` |
| `src/hooks/useApiResource.js` | Gom logic load/fallback/dashboard-employees | Không còn hai bản sao của `useEffect` fetch |
| `src/components/PageContent.jsx` | Wrapper nội dung chung | Giảm markup lặp trong các page đã refactor |
| `AppRoutes.jsx` | Tách route public khỏi protected layout; Settings cũng cần đăng nhập | Login/register không còn render sidebar/topbar/chat; toàn bộ app area có cùng policy |
| `ChatWidget`, `LoginPage`, `RegisterPage` | Chuyển sang service API chuẩn | Không thay đổi luồng nghiệp vụ, chỉ đổi điểm gọi |
| `DashboardPage`, `EmployeesPage` | Dùng hook chung và fallback đúng shape | Tránh lỗi UI lúc backend offline |
| `DashboardView`, `EmployeesView` | Tương thích tạm thời `dept/departments`, `permission/permissions`; render an toàn | Bảo vệ UI trong giai đoạn migration dữ liệu/API |
| `Leftbar`, `Topbar`, form login/register | Đổi action từ `div` sang `button`, thêm label/ARIA/autocomplete | Cải thiện keyboard navigation và accessibility |

Không component hay màn hình nào bị xoá; không có feature mới. Thay đổi layout ở public route là tổ chức route: các component cũ vẫn giữ nguyên và chỉ được render cho phiên đăng nhập hợp lệ.

## Việc cần loại bỏ hoặc cải thiện tiếp (chưa xoá trong lần này)

| Mức độ | Mục | Khuyến nghị |
| --- | --- | --- |
| Cao | Production thiếu `VITE_API_BASE_URL` | Production workflow đã fail-fast khi repository variable này rỗng; đặt nó bằng public NestJS `/api` URL. |
| Cao | Token JWT trong `localStorage` | Với production có yêu cầu bảo mật cao, chuyển sang HttpOnly/Secure/SameSite cookie, CSRF protection và refresh-token flow. Giữ localStorage hiện tại để không đổi feature/contract. |
| Trung bình | Compatibility field sau migration | Sau khi Mongo/NestJS contract ổn định, bỏ nhánh fallback `dept/permission` cũ và giữ một response DTO duy nhất. |
| Trung bình | `main.css` không được import và `src/public/Logo.png` trùng asset | Xác nhận không còn được dùng rồi xoá ở commit cleanup riêng; không xoá trong refactor này. |
| Trung bình | Comment code cũ/đã vô hiệu hoá trong pages và Leftbar | Xoá sau khi lịch sử Git đã lưu; dùng issue/tài liệu thay cho dead code. |
| Trung bình | Link `/help` trong Leftbar chưa có route | Hoặc tạo route khi có yêu cầu sản phẩm, hoặc bỏ item navigation ở commit feature/cleanup riêng. |
| Trung bình | CSS còn lặp màu, spacing, form/card | Thiết lập token (`--color-*`, `--space-*`, `--radius-*`) và component CSS dùng chung; không cần đưa CSS framework ngay. |
| Trung bình | React/Vite phiên bản cũ | Lên kế hoạch nâng lần lượt React 18/19 và Vite hiện hành, kiểm thử build/Router trước khi merge. Không nâng dependency trong lần refactor này. |
| Thấp | Inline style ở placeholder/loading và `EyeIcon` | Chuyển sang class CSS khi chuẩn hoá design system. |
| Thấp | Chưa có test frontend/lint config | Bổ sung ESLint, Prettier, React Testing Library và test cho auth/API error ở một thay đổi chuyên biệt. |

## Cấu trúc mục tiêu nên giữ

```text
src/
  README.md           # quy ước `.jsx`/`.js` và hướng phụ thuộc
  assets/             # ảnh, font đóng gói cùng bundle
  components/         # UI dùng lại, không sở hữu business flow
  config/              # endpoint/cấu hình môi trường
  hooks/               # custom React hooks
  layouts/             # composition khung màn hình
  pages/               # điều phối state, navigation, use case
  services/            # HTTP client, API modules, persistence nhỏ
  styles/              # global tokens và CSS theo component/feature
  views/               # presentation nhận props từ page
```

Ở cấp `frontend/`, `README.md` mô tả scripts, môi trường và deployment; `src/README.md` mô tả source. Tách tài liệu theo tầng này để thay đổi UI không bị lẫn với hướng dẫn vận hành package.

Khi domain phát triển lớn hơn, thay vì để `pages/`, `views/`, `services/` phình to, có thể chuyển dần sang `features/auth`, `features/employees`, `features/dashboard`; không cần đổi ngay khi project còn nhỏ.

## Kiểm thử frontend tối thiểu trước khi merge

```powershell
cd frontend
npm ci
npm run build
```

Sau build, kiểm tra thủ công: vào `/#/login` khi chưa có token, login thành công, refresh dashboard, mở Employees khi backend offline, gửi chat, logout, và thử Tab/Enter trên menu/avatar/password visibility.
