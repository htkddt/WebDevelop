#!/usr/bin/env bash
# Start python_ai/server (port 5000) and fe/ Vite (port 8000). Ctrl+C stops both.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVER_DIR="$ROOT/python_ai/server"
FE_DIR="$ROOT/fe"

if [[ ! -f "$SERVER_DIR/app.py" ]]; then
  echo "Missing $SERVER_DIR/app.py"
  exit 1
fi
if [[ ! -f "$FE_DIR/package.json" ]]; then
  echo "Missing $FE_DIR/package.json"
  exit 1
fi

if [[ -f "$SERVER_DIR/.venv/bin/activate" ]]; then
  # shellcheck source=/dev/null
  source "$SERVER_DIR/.venv/bin/activate"
fi

if ! python3 -c "import flask, flask_cors" 2>/dev/null; then
  echo "==> Installing Python server deps (pip install -r python_ai/server/requirements.txt)"
  python3 -m pip install -r "$SERVER_DIR/requirements.txt"
fi

cleanup() {
  if [[ -n "${SERVER_PID:-}" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill "$SERVER_PID" 2>/dev/null || true
  fi
  if [[ -n "${FE_PID:-}" ]] && kill -0 "$FE_PID" 2>/dev/null; then
    kill "$FE_PID" 2>/dev/null || true
  fi
  wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

if [[ ! -d "$FE_DIR/node_modules" ]]; then
  echo "==> npm install in fe/ (first run)"
  (cd "$FE_DIR" && npm install)
fi

echo "==> Starting API http://127.0.0.1:5000"
(
  cd "$SERVER_DIR"
  exec python3 app.py
) &
SERVER_PID=$!

echo "==> Starting Vite http://127.0.0.1:8000"
(
  cd "$FE_DIR"
  exec npm run dev
) &
FE_PID=$!

echo "Open http://127.0.0.1:8000  —  Press Ctrl+C to stop both."
wait
