# Frontend Architecture and Coding Standards

## Current state

| Area | Details |
| --- | --- |
| Runtime | React 17, Vite 2.9, React Router 6.3 |
| UI dependency | `lucide-react` |
| Screens | Nine pages and views: login, registration, dashboard, employees, leave, report, profile, assets, and settings |
| Shared shell | `MainLayout`, `Topbar`, `Leftbar`, `ChatWidget`, and `ProtectedRoute` |
| Styling | Feature-oriented plain CSS; no complete shared token/component system yet |
| Routing | `HashRouter`, suitable for GitHub Pages |
| API | Local NestJS API: `http://localhost:5000/api`; production injects `VITE_API_BASE_URL` ending in `/api` |
| Tests | Jest and React Testing Library tests live in `tests/frontend/` |

See [frontend/README.md](frontend/README.md) for package operation and [frontend/src/README.md](frontend/src/README.md) for React/JavaScript source conventions.

## Long-term coding rules

1. Use function components, PascalCase component names, and camelCase variable/function names. A page, view, or component file should normally have one primary default export.
2. Preserve layers: `views/` render UI from props; `pages/` orchestrate state and use cases; `services/` make HTTP calls and store sessions; `config/` owns environment configuration; `hooks/` hold reusable React logic.
3. Do not scatter API URLs, `fetch`, `localStorage` keys, or HTTP headers through pages/components. Requests go through `services/apiClient.js`; endpoints belong in `config/api.js`.
4. Use `VITE_*` variables such as `VITE_API_BASE_URL` for environment-specific configuration. Do not hard-code a production host in a component.
5. JSON APIs send `Content-Type: application/json`; tokens use `Authorization: Bearer <token>`. Do not use a custom `type` header for JSON.
6. Use semantic HTML for interaction: `button` for actions, `NavLink`/`Link` for navigation, `label htmlFor` for form inputs, and `aria-label` for icon-only buttons.
7. Async state must cover loading, success, and error/fallback; do not set state after an unmount.
8. Treat backend props as untrusted: use fallbacks/null guards and do not call methods such as `.charAt()` on a potentially missing value.
9. Avoid large inline styles. Prefer CSS classes, global custom properties, and a component/feature prefix.
10. Save source in UTF-8. Garbled non-ASCII text in a terminal is a cue to verify encoding before editing.
11. Add a regression test for every bug fix and tests for newly introduced conditional logic or user-visible state.

## Applied design changes

| Location | Change | Reason |
| --- | --- | --- |
| `src/config/api.js` | Centralized the base URL and endpoints; supports `VITE_API_BASE_URL`. | Prevents repetition of local/production URLs. |
| `src/services/apiClient.js` | One JSON client normalizes errors and attaches a Bearer token. | Keeps headers correct and compatible with NestJS guards. |
| `src/services/api.js` | Separates auth, dashboard, employee, and chat APIs. | Pages orchestrate use cases instead of HTTP details. |
| `src/services/authStorage.js` | Centralizes session keys and access. | Avoids magic `token`/`user` strings. |
| `src/hooks/useApiResource.js` | Shares dashboard/employee load and fallback behavior. | Removes duplicate `useEffect` request logic. |
| `src/components/PageContent.jsx` | Provides a shared content wrapper. | Reduces repeated page markup. |
| `AppRoutes.jsx` | Separates public routes from the protected layout; Settings is protected. | Public pages do not render the authenticated shell. |
| Chat, login, registration pages | Use the shared API services. | Preserves business flow while changing the call boundary. |
| Dashboard and employees pages | Use the shared hook and schema-safe fallbacks. | Keeps the UI usable while the API is offline or data is transitional. |
| Dashboard and employees views | Temporarily support `dept/departments` and `permission/permissions`; render defensively. | Protects the UI during data/API migration. |
| Navigation and forms | Use buttons, labels, ARIA, and autocomplete correctly. | Improves keyboard navigation and accessibility. |
| `tests/frontend/` | Covers components, routes, resources, forms, navigation, and browser API clients. | Keeps regressions isolated from backend infrastructure. |

No screens or components were removed and no UI feature was introduced by the refactor. The public-route layout change only controls when the existing application shell is rendered.

## Follow-up work

| Priority | Item | Recommendation |
| --- | --- | --- |
| High | Production API configuration | Set `VITE_API_BASE_URL` to the public HTTPS NestJS URL ending in `/api`; GitHub Pages deployment is skipped until this is configured. |
| High | JWT storage in `localStorage` | For higher-security production requirements, move to HttpOnly/Secure/SameSite cookies with CSRF protection and a refresh-token flow. |
| Medium | Temporary compatibility fields | Remove the old `dept/permission` fallback once MongoDB and NestJS responses are stable. |
| Medium | Duplicate/unused assets | Confirm `main.css` and `src/public/Logo.png` are unused, then remove them in a dedicated cleanup commit. |
| Medium | Dead comments and `/help` link | Remove obsolete comments and either implement or remove the navigation item in a scoped change. |
| Medium | Repeated CSS values | Introduce `--color-*`, `--space-*`, and `--radius-*` tokens before considering a CSS framework. |
| Medium | Old React/Vite versions | Plan staged upgrades and test the build, router, and suite before merging. |
| Low | Inline placeholder/loading styles | Move them to CSS when the design system is standardized. |
| Low | Tooling | Add ESLint and Prettier in a dedicated tooling change; Jest/Testing Library coverage is now present. |

## Target source layout

```text
src/
  README.md           # `.jsx`/`.js` conventions and dependency direction
  assets/             # images and fonts bundled with the application
  components/         # reusable UI without business-flow ownership
  config/             # endpoints and environment configuration
  hooks/              # custom React hooks
  layouts/            # screen-shell composition
  pages/              # state, navigation, and use-case orchestration
  services/           # HTTP client, API modules, small persistence
  styles/             # global tokens and component/feature CSS
  views/              # presentation components that receive page props
```

`frontend/README.md` documents scripts, environment variables, and deployment; `frontend/src/README.md` documents source structure. Keep those concerns separate. If the domain grows, migrate incrementally to feature folders such as `features/auth`, `features/employees`, and `features/dashboard` rather than allowing the current layers to become large.

## Minimum pre-merge check

```powershell
cd frontend
npm ci
npm run build
npm test
```

Afterward, manually verify login, successful navigation to the dashboard, dashboard refresh, the employees fallback while the API is offline, chat submission, logout, and Tab/Enter behavior on navigation, avatar, and password controls.
