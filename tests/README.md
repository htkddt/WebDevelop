# Test suite

All regression tests live here so a new feature or bug fix can add its matching test beside the relevant layer:

- `frontend/`: React components, routing, hooks and browser API clients.
- `nestjs/`: NestJS services and their boundary contracts, using mocks instead of MongoDB or the AI service.
- `ai_service/`: FastAPI routes, validation, authentication and the Ollama provider, using mocked HTTP.

Run every suite from the repository root:

```powershell
& "$env:LOCALAPPDATA\179FC\tools\node-v20.20.2-win-x64\npm.cmd" --prefix frontend test
& "$env:LOCALAPPDATA\179FC\tools\node-v20.20.2-win-x64\npm.cmd" --prefix backend\nestjs_ai test
Push-Location backend\ai_service; .\.venv\Scripts\python.exe -m pytest ..\..\tests\ai_service -q; Pop-Location
```

The tests are intentionally isolated: they do not require MongoDB, Ollama, service credentials, or a running browser. Add a regression test before (or alongside) every bug fix and add unit tests for every newly introduced branch or user-visible state.
