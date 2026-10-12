# GitHub Actions YAML và CI/CD

## Mục tiêu pipeline của project

```text
feature branch push
  ├─ CI: frontend-build, nest-build, ai-test
  └─ Preview deployment: artifact, sau đó gọi preview host nếu có hook

pull request -> main/master
  └─ CI bắt buộc pass trước khi merge

merge pull request -> main/master
  ├─ chạy lại ba kiểm tra
  ├─ deploy frontend lên GitHub Pages
  └─ trigger backend host bằng deploy hook (nếu đã cấu hình)
```

Workflow ở `.github/workflows/` đã được tách thành reusable workflow để frontend, NestJS và FastAPI có thể được build/test độc lập nhưng vẫn được gọi trong pipeline tổng. GitHub chỉ nhận workflow đặt trực tiếp trong thư mục này; không đặt reusable workflow ở thư mục con.

Kiến trúc hiện tại của `.github/`:

```text
.github/
├── branch-protection.md          # thao tác Ruleset/PR-only merge thực hiện trên GitHub
└── workflows/
    ├── ci.yml                    # checks cho branch và pull request
    ├── preview-deploy.yml        # artifact/preview cho sub-branch
    ├── deploy-production.yml     # deploy sau merge vào main/master
    ├── reusable-frontend.yml     # npm ci + Vite build
    ├── reusable-nest.yml         # npm ci + NestJS build
    └── reusable-ai.yml           # Python dependency + FastAPI tests
```

Các workflow trỏ đúng hai backend runtime folders `backend/nestjs_ai` và `backend/ai_service`; không còn workflow cho Express hoặc Flask legacy.

## Cú pháp YAML cơ bản cho GitHub Actions

YAML dùng indentation để biểu diễn cấu trúc. Dùng 2 spaces nhất quán, không dùng tab. Key-value dùng `:`, danh sách dùng `-`, string chứa ký tự đặc biệt nên được quote.

```yaml
name: CI

on:
  pull_request:
    branches: [main, master]

permissions:
  contents: read

jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Install dependencies
        run: npm ci
```

Các key quan trọng:

| Key | Mục đích |
| --- | --- |
| `name` | Tên workflow/check hiển thị trên GitHub. |
| `on` | Event khởi chạy: `push`, `pull_request`, `workflow_dispatch`, `workflow_call`. |
| `permissions` | Cấp quyền tối thiểu cho `GITHUB_TOKEN`. |
| `env` | Environment variable không nhạy cảm hoặc map secret vào môi trường step/job. |
| `jobs` | Nhóm công việc có runner riêng. |
| `runs-on` | Runner, ví dụ `ubuntu-latest`. |
| `steps` | Các action/command chạy tuần tự trong một job. |
| `uses` | Dùng action hoặc gọi reusable workflow. |
| `run` | Chạy shell command. |
| `needs` | Dependency giữa các job. |
| `if` | Điều kiện chạy; dùng `env.SECRET_NAME` thay vì tham chiếu secret trực tiếp. |
| `concurrency` | Hủy run cũ khi commit mới đến cùng branch/environment. |

Ví dụ biến và expression:

```yaml
env:
  DEPLOY_HOOK: ${{ secrets.DEPLOY_HOOK }}

steps:
  - name: Deploy only when configured
    if: ${{ env.DEPLOY_HOOK != '' }}
    run: curl --fail --request POST "$DEPLOY_HOOK"
```

Không in `secrets.*`, `.env`, token, MongoDB URI hoặc request body có password ra log.

## Ràng buộc: step/job nào chạy trước

Trong **một job**, các `steps` chạy tuần tự. Một step thất bại sẽ làm job thất bại và step sau mặc định không chạy. `if: ${{ always() }}` chỉ dùng cho việc cleanup/upload log vì nó chạy dù step trước thất bại.

Các **job** chạy song song nếu không có `needs`. Khai báo `needs` để tạo dependency và chỉ triển khai sau khi build/test thành công:

```yaml
jobs:
  build:
    runs-on: ubuntu-latest
    steps: []

  test:
    needs: build
    runs-on: ubuntu-latest
    steps: []

  deploy:
    needs: [build, test]
    if: ${{ success() }}
    runs-on: ubuntu-latest
    steps: []
```

Trong repository này, `deploy-frontend` cần `frontend`, `nestjs-api` và `ai-service`. Như vậy frontend không được deploy nếu API hoặc AI service không build/test được. Artifact chỉ tồn tại trong workflow run, nên bước deploy phải tải artifact bằng đúng tên đã upload.

## Branch, pull request và deploy policy

Các workflow hiện tại:

| File | Trigger | Trách nhiệm |
| --- | --- | --- |
| `ci.yml` | Mọi push vào branch không phải `main/master`, PR vào `main/master` | Gọi ba reusable build/test job. |
| `preview-deploy.yml` | Mọi push branch không phải `main/master` | Build frontend và cung cấp preview artifact; gọi `PREVIEW_DEPLOY_HOOK` khi host preview đã được cấu hình. |
| `deploy-production.yml` | Push vào `main/master` sau merge | Kiểm tra lại toàn bộ, deploy GitHub Pages, gọi `BACKEND_DEPLOY_HOOK` nếu có. |
| `reusable-frontend.yml` | `workflow_call` | `npm ci`, build Vite, upload `frontend-dist`. |
| `reusable-nest.yml` | `workflow_call` | `npm ci`, compile NestJS. |
| `reusable-ai.yml` | `workflow_call` | Cài Python dependency, chạy FastAPI tests. |

Không thể ngăn direct push vào `main/master` chỉ bằng YAML. Bắt buộc cấu hình GitHub **Ruleset** hoặc branch protection theo [`.github/branch-protection.md`](.github/branch-protection.md): require pull request, approval, check `frontend-build`/`nest-build`/`ai-test`, branch up-to-date, block force push và restrict push. GitHub branch rules mới là nơi thực thi yêu cầu “không commit thẳng master”.

GitHub Pages chỉ có một deployment production; không an toàn nếu mỗi feature branch ghi đè site đó. Vì vậy feature branch có `frontend-dist` artifact và hỗ trợ deploy preview thật khi thêm repository secret `PREVIEW_DEPLOY_HOOK` của Vercel, Netlify, Render hoặc host đã chọn. Tương tự, set `BACKEND_DEPLOY_HOOK` hoặc bật Git integration trên host để deploy NestJS/FastAPI sau merge.

Production workflow cũng fail-fast nếu repository variable `VITE_API_BASE_URL` chưa được đặt. Giá trị này phải là public NestJS URL có hậu tố `/api`; Vite nhúng nó vào frontend tại build time.

## Tách workflow nhỏ và gọi từ workflow tổng

Một reusable workflow phải có `on: workflow_call`:

```yaml
# .github/workflows/reusable-example.yml
name: Reusable example
on:
  workflow_call:

jobs:
  check:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - run: echo "reusable job"
```

Workflow caller không dùng `runs-on`/`steps` trong job gọi; nó dùng `uses`:

```yaml
jobs:
  example:
    uses: ./.github/workflows/reusable-example.yml
```

Lợi ích: setup Node/Python/cache chỉ viết một lần, job name cố định để branch protection chọn được, và production/PR/preview dùng cùng lệnh build. Chỉ truyền secret cho reusable workflow khi thật sự cần; không dùng `secrets: inherit` mặc định nếu không cần thiết.

## Lỗi thường gặp và cách tránh

| Vấn đề | Nguyên nhân / cách xử lý |
| --- | --- |
| `mapping values are not allowed` | Sai indentation, dùng tab, hoặc quên quote string có `:`. Kiểm tra bằng YAML linter. |
| Workflow không xuất hiện | File không ở `.github/workflows/*.yml`, sai `on`, hoặc YAML invalid. |
| Job deploy chạy trước test | Thiếu `needs` hoặc `if: success()`. |
| Required check bị Pending mãi | Workflow bị bỏ qua bởi `paths`, branch filter hoặc `[skip ci]`; tránh path filter cho check bắt buộc. |
| `npm ci` fail | `package.json` và `package-lock.json` không đồng bộ; cập nhật lockfile cùng dependency. |
| Artifact không tìm thấy | Tên upload/download khác nhau, artifact upload sau step fail, hoặc dùng sai run. |
| Secret rỗng ở PR từ fork | GitHub không đưa repository secret vào fork PR; test không cần secret và deploy chỉ chạy sau merge/internal branch. |
| `403 Resource not accessible` | `permissions` chưa đủ; cấp đúng scope tối thiểu cho workflow/job. |
| Pages deploy bị ghi đè | Không deploy GitHub Pages từ mọi branch; dùng preview host/hook riêng. |
| Direct push vẫn thành công | Chưa tạo Ruleset/branch protection, hoặc admin có quyền bypass. |

Tham khảo chính thức: [workflow syntax](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax), [reusable workflows](https://docs.github.com/en/actions/how-tos/reuse-automations/reuse-workflows), và [protected branches/rulesets](https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-protected-branches/about-protected-branches).
