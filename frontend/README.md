# Frontend

The frontend is a React and Vite application. It renders the user interface and calls only the NestJS public API; it never connects directly to MongoDB, the FastAPI AI service, or Ollama.

## Structure and responsibilities

| Location | Responsibility |
| --- | --- |
| `src/` | React source; see [src/README.md](src/README.md) for structure and conventions. |
| `index.html` | Vite HTML entry point. |
| `vite.config.js` | Vite configuration and GitHub Pages base path. |
| `package.json` | Frontend scripts and dependencies. |
| `dist/` | Local build output; do not edit it as source. |

## Run locally

```powershell
cd frontend
npm ci
npm run dev
```

The local API default is `http://localhost:5000/api`. As `vite.config.js` uses `base: '/WebDevelop/'`, open `http://localhost:3000/WebDevelop/index.html#/login`.

Create `frontend/.env.local` if the API has a different host:

```dotenv
VITE_API_BASE_URL=http://localhost:5000/api
```

Only variables prefixed with `VITE_` are exposed in the client bundle. Never put a token, password, MongoDB URI, or service secret in one.

## Build and test

```powershell
npm run build
npm test
npm run test:watch
npm run preview
```

`npm run build` and `npm test` are required CI checks. Jest runs the React suite in `../tests/frontend`; the suite uses mocks and needs neither a running API nor a browser. In production, the `VITE_API_BASE_URL` repository variable must be the public NestJS URL ending in `/api`.

## Change guidelines

- Keep `HashRouter`, because the frontend deploys to GitHub Pages.
- Route API URLs, HTTP headers, and session storage through `src/config/` and `src/services/`; pages and views must not hard-code endpoints.
- Update `src/config/api.js`, root [README.md](../README.md), and [Backend.md](../Backend.md) whenever the public API changes.
- Add or update a regression test in `tests/frontend/` for each bug fix or user-visible state.
- Do not add a feature or change a public contract solely as part of a coding-design refactor.
