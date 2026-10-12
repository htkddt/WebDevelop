# Local Build and Run Guide

This guide records a verified local setup for `179FC WebDevelop`.

## Architecture and startup order

```text
MongoDB (27017)
      │
      ├── NestJS public API (5000/api) ──> FastAPI AI service (8000) ──> Ollama (11434)
      │
      └── React/Vite frontend (3000) ──> NestJS public API
```

Start MongoDB first, then Ollama when testing real chat, FastAPI, NestJS, and finally React/Vite. The browser calls only NestJS; it never calls MongoDB, FastAPI, or Ollama directly.

## Prerequisites

| Component | Verified local setup |
| --- | --- |
| Node.js | Node 20.20.2 for build, test, and run. System Node 14.17.0 is not compatible with the current lockfiles/NestJS stack. |
| Python | Python 3.12.7 runs FastAPI and its tests. CI uses Python 3.11. |
| MongoDB | MongoDB Server 8.2.6. |
| Ollama | `llama3:latest`, with `OLLAMA_MODEL=llama3`. |

CI uses Node 20. Install it when the machine only has Node 14:

```powershell
winget install --id OpenJS.NodeJS.20 --exact --accept-package-agreements --accept-source-agreements
```

For a portable Node 20 installation, prepend its path in each PowerShell session:

```powershell
$node20 = "$env:LOCALAPPDATA\179FC\tools\node-v20.20.2-win-x64"
$env:Path = "$node20;$env:Path"
node --version # must be v20.x
```

## Environment configuration

These ignored files contain local values; never commit them or their secrets.

`backend/ai_service/.env`:

```dotenv
AI_SERVICE_TOKEN=<local-internal-token>
OLLAMA_URL=http://localhost:11434/api/generate
OLLAMA_MODEL=llama3
REQUEST_TIMEOUT_SECONDS=30
MAX_MESSAGE_CHARACTERS=8000
```

`backend/nestjs_ai/.env`:

```dotenv
PORT=5000
MONGO_URL=mongodb://127.0.0.1:27017/179FC
JWT_SECRET=<long-local-development-secret>
FRONTEND_ORIGIN=http://localhost:3000
AI_SERVICE_URL=http://127.0.0.1:8000
AI_SERVICE_TOKEN=<same-local-internal-token>
```

`frontend/.env.local`:

```dotenv
VITE_API_BASE_URL=http://localhost:5000/api
```

`AI_SERVICE_TOKEN` must match between the backend services and must differ from `JWT_SECRET`.

## Install, build, and test

Run these commands from `D:\WebDevelop` after preparing the virtual environment and Node 20.

```powershell
cd backend\ai_service
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install --disable-pip-version-check -r requirements.txt
.\.venv\Scripts\python.exe -m pytest ..\..\tests\ai_service -q

cd ..\nestjs_ai
npm ci --no-audit --no-fund
npm run build
npm test

cd ..\..\frontend
npm ci --no-audit --no-fund
npm run build
npm test
```

All suites are isolated from MongoDB, Ollama, credentials, and a running browser. See [tests/README.md](tests/README.md) for the test layout. Vite may emit a harmless `use client` directive warning while building dependencies.

## Start the full stack

Use separate PowerShell terminals.

### Terminal 1 — MongoDB

```powershell
$mongoData = "$env:LOCALAPPDATA\179FC\mongodb\data"
New-Item -ItemType Directory -Force -Path $mongoData | Out-Null
& 'C:\Program Files\MongoDB\Server\8.2\bin\mongod.exe' --dbpath $mongoData --bind_ip 127.0.0.1 --port 27017
```

### Terminal 2 — FastAPI AI service

```powershell
cd D:\WebDevelop\backend\ai_service
.\.venv\Scripts\python.exe -m uvicorn app.main:app --host 127.0.0.1 --port 8000
Invoke-RestMethod http://127.0.0.1:8000/healthz
```

The health check should return `status: ok`.

### Terminal 3 — NestJS API

```powershell
cd D:\WebDevelop\backend\nestjs_ai
$node20 = "$env:LOCALAPPDATA\179FC\tools\node-v20.20.2-win-x64"
$env:Path = "$node20;$env:Path"
npm run start:dev
```

The API listens at `http://localhost:5000/api`. Calling `/api/dashboard` without a Bearer JWT should return `401 Unauthorized`.

### Terminal 4 — Frontend

```powershell
cd D:\WebDevelop\frontend
$node20 = "$env:LOCALAPPDATA\179FC\tools\node-v20.20.2-win-x64"
$env:Path = "$node20;$env:Path"
npm run dev -- --host 127.0.0.1
```

The configured Vite base path means the verified local URL is `http://localhost:3000/WebDevelop/index.html#/login`.

## Check and stop services

```powershell
Get-NetTCPConnection -State Listen -LocalPort 27017,8000,5000,3000 |
  Sort-Object LocalPort |
  Select-Object LocalAddress, LocalPort, OwningProcess
```

Stop each service with `Ctrl+C`. If a PID must be stopped, inspect it first and stop only the process you started:

```powershell
Stop-Process -Id <pid-mongodb>, <pid-fastapi>, <pid-nest>, <pid-frontend>
```
