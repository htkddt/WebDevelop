# React Source

`src/` contains the user-interface source. These conventions keep React modules that render JSX distinct from plain JavaScript and maintain a clear dependency direction.

## Source map

```text
src/
├── assets/       # assets bundled by Vite
├── components/   # reusable UI: navigation, chat, guard, wrapper
├── config/       # environment configuration and API endpoints
├── hooks/        # reusable React hooks
├── layouts/      # shared layout for the authenticated area
├── pages/        # state, navigation, and use-case orchestration
├── services/     # HTTP client, API modules, session storage
├── styles/       # global and component/feature CSS
├── views/        # presentation components that receive page props
├── App.jsx       # application root
├── AppRoutes.jsx # public and protected route declarations
└── main.jsx      # React entry point
```

`src/public/` contains existing project assets. Prefer `assets/` for new assets that should be managed and URL-hashed by Vite.

## Why both `.jsx` and `.js`?

Vite can transform JSX in either extension, but this project uses the extension to communicate intent:

- **`.jsx`**: React modules that render JSX, including `AppRoutes.jsx`, pages, views, layouts, and components.
- **`.js`**: JavaScript modules that do not render JSX, including `config/api.js`, `services/apiClient.js`, `services/authStorage.js`, and helpers.

Do not rename every module merely to make extensions uniform. If a `.js` module begins returning JSX, rename it to `.jsx`; if a `.jsx` module no longer renders JSX, rename it to `.js` in the same change and update its imports.

## Dependency rules

```text
views/components → props and callbacks from pages/layouts
pages/layouts     → hooks, services, views/components
hooks             → services
services          → config
config            → environment variables (VITE_*)
```

- `views/` only present UI; they do not call `fetch`, read `localStorage`, or hard-code URLs.
- `pages/` own loading, error, and fallback states before passing data to views.
- Only `services/apiClient.js` sends HTTP requests; it obtains the token from `authStorage.js`.
- `config/api.js` owns endpoint and base-URL definitions.
- Interactive components use semantic elements such as `button`, `label`, and `NavLink`, with accessible labels where appropriate.
