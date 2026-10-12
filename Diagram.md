# Architecture Diagrams

The diagrams reflect the current architecture: NestJS is the only public API and FastAPI is the chatbot's private service.

## Folder ownership

```mermaid
flowchart TB
  Root[Repository root]
  Frontend[frontend/\nREADME: package, environment, deployment]
  Source[frontend/src/\nREADME: React/JS conventions]
  Backend[backend/\nREADME: cross-service operations]
  Nest[backend/nestjs_ai/\nREADME: public API]
  AI[backend/ai_service/\nREADME: private AI service]
  Tests[tests/\nREADME: isolated regression suites]
  Github[.github/\nworkflows + branch-protection.md]
  Docs[Frontend.md / Backend.md / Diagram.md / Yaml.md]

  Root --> Frontend --> Source
  Root --> Backend
  Backend --> Nest
  Backend --> AI
  Root --> Tests
  Root --> Github
  Root --> Docs
```

The legacy runtime folders `backend/nodejs_ai/` and `backend/python_ai/` are not part of the current application flow.

## Frontend, API, and AI service

```mermaid
flowchart LR
  Browser[Browser / HashRouter]

  subgraph Frontend[React + Vite frontend]
    Main[main.jsx]
    Routes[AppRoutes]
    Guard[ProtectedRoute]
    Public[LoginPage / RegisterPage]
    Layout[MainLayout]
    Shared[Topbar + Leftbar + ChatWidget]
    Pages[DashboardPage / EmployeesPage / other Pages]
    Views[DashboardView / EmployeesView / other Views]
    API[config/api + services/apiClient + services/api]
    Storage[authStorage / localStorage]

    Main --> Routes
    Routes --> Public
    Routes --> Guard --> Layout
    Layout --> Shared
    Layout --> Pages --> Views
    Public --> API
    Pages --> API
    Shared --> API
    API <--> Storage
    Guard --> Storage
  end

  subgraph PublicAPI[NestJS public API]
    Auth[Auth module]
    Users[Users + Dashboard modules]
    Gateway[Chat gateway]
    Mongo[(MongoDB)]
    Auth --> Mongo
    Users --> Mongo
  end

  subgraph PrivateAI[FastAPI private AI service]
    Health[GET /healthz]
    Chat[POST /v1/chat/completions]
    Provider[Ollama provider adapter]
    Model[Ollama / future LLM provider]
    Chat --> Provider --> Model
  end

  Browser --> Main
  API -->|HTTPS /api + Bearer JWT| Auth
  API -->|HTTPS /api + Bearer JWT| Users
  API -->|HTTPS /api + Bearer JWT| Gateway
  Gateway -->|private HTTP + service token| Chat
```

## Authenticated-area composition

```mermaid
flowchart TB
  App[App]
  Router[AppRoutes]
  Guard[ProtectedRoute]
  Layout[MainLayout]
  Topbar[Topbar]
  Leftbar[Leftbar]
  Outlet[Outlet]
  Chat[ChatWidget]
  Pages[Page component]
  Views[View component]

  App --> Router
  Router -->|public| Login[LoginPage / RegisterPage]
  Router -->|private| Guard --> Layout
  Layout --> Topbar
  Layout --> Leftbar
  Layout --> Outlet --> Pages --> Views
  Layout --> Chat
```

## Login and dashboard sequence

```mermaid
sequenceDiagram
  actor User
  participant LV as LoginView
  participant LP as LoginPage
  participant Auth as authApi/apiClient
  participant Nest as NestJS /api
  participant Store as authStorage
  participant Mongo as MongoDB
  participant Router as HashRouter
  participant DP as DashboardPage/useApiResource

  User->>LV: Submit email + password
  LV->>LP: onLogin(credentials)
  LP->>Auth: POST /auth/login (JSON)
  Auth->>Nest: public request
  Nest->>Mongo: find user + verify password
  Mongo-->>Nest: user
  Nest-->>Auth: token + user
  Auth-->>LP: session response
  LP->>Store: setSession(token, user)
  LP->>Router: navigate('/dashboard')
  Router->>DP: render protected page
  DP->>Auth: GET /dashboard + Bearer token
  Auth->>Nest: authenticated request
  Nest->>Mongo: aggregate statistics
  Mongo-->>Nest: statistics
  Nest-->>DP: departments, total, active, inactive
```

## Chat sequence

```mermaid
sequenceDiagram
  actor User
  participant Widget as ChatWidget
  participant API as chatApi/apiClient
  participant Nest as NestJS /api/chat
  participant AI as FastAPI AI service
  participant Model as Ollama

  User->>Widget: Send message
  Widget->>Widget: Append local user message and show typing
  Widget->>API: POST /chat { contents }
  API->>Nest: JSON + Bearer JWT
  Nest->>Nest: JWT guard + ChatRequestDto validation
  Nest->>AI: POST /v1/chat/completions + service token
  AI->>AI: Validate ChatRequest
  AI->>Model: generate(prompt, stream=false)
  Model-->>AI: model response
  AI-->>Nest: reply + conversation_id
  Nest-->>API: header, reply, conversationId
  API-->>Widget: chatbot payload
  Widget->>Widget: Append bot response and hide typing
```

FastAPI provides the boundary for adding RAG, memory, streaming, queues, and provider routing without changing the frontend's public contract.
