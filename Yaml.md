# GitHub Actions YAML and CI/CD

## Pipeline goals

```text
feature-branch push
  ├─ CI: frontend build/test, NestJS build/test, FastAPI test
  └─ Preview deployment: artifact, then optional preview-host hook

pull request → main/master
  └─ the same CI checks must pass before merge

pull-request merge → main/master
  ├─ run all checks again
  ├─ deploy the frontend to GitHub Pages
  └─ trigger backend hosting through an optional deployment hook
```

The workflows in `.github/workflows/` use reusable workflows so the frontend, NestJS, and FastAPI can be built/tested independently while still participating in shared pipelines. GitHub recognizes workflow files only when they are placed directly in this folder.

```text
.github/
├── branch-protection.md          # Ruleset/PR-only merge setup in GitHub
└── workflows/
    ├── ci.yml                    # branch and pull-request checks
    ├── preview-deploy.yml        # feature-branch artifact/preview
    ├── deploy-production.yml     # deployment after a main/master push
    ├── reusable-frontend.yml     # npm ci + Vite build + Jest tests
    ├── reusable-nest.yml         # npm ci + NestJS build + Jest tests
    └── reusable-ai.yml           # Python dependencies + pytest suite
```

All workflows target the active backend runtime folders, `backend/nestjs_ai` and `backend/ai_service`; there are no Express or Flask legacy workflow jobs.

## GitHub Actions YAML basics

YAML uses indentation to express structure. Use two spaces consistently, never tabs. Use `:` for key/value pairs, `-` for lists, and quote strings containing special characters when needed.

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

| Key | Purpose |
| --- | --- |
| `name` | Displayed workflow/check name. |
| `on` | Trigger event: `push`, `pull_request`, `workflow_dispatch`, or `workflow_call`. |
| `permissions` | Minimum permissions for `GITHUB_TOKEN`. |
| `env` | Non-sensitive environment variables or a secret mapped into a job/step environment. |
| `jobs` | Work groups, each with its own runner. |
| `runs-on` | Runner image, such as `ubuntu-latest`. |
| `steps` | Commands/actions run sequentially inside a job. |
| `uses` | Use an action or reusable workflow. |
| `run` | Run a shell command. |
| `needs` | Declare job dependencies. |
| `if` | Conditionally run work; prefer an `env` value over reading a secret directly. |
| `concurrency` | Cancel an obsolete run when a newer commit reaches the same branch/environment. |

```yaml
env:
  DEPLOY_HOOK: ${{ secrets.DEPLOY_HOOK }}

steps:
  - name: Deploy only when configured
    if: ${{ env.DEPLOY_HOOK != '' }}
    run: curl --fail --request POST "$DEPLOY_HOOK"
```

Never print `secrets.*`, `.env` content, tokens, MongoDB URIs, or request bodies containing passwords to logs.

## Job ordering and dependencies

Within one job, steps are sequential: a failed step fails the job and normally prevents later steps from running. Use `if: ${{ always() }}` only for cleanup or diagnostic uploads that must run after a failure.

Jobs run in parallel unless they declare `needs`. Use dependencies to deploy only after successful validation:

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

In this repository, `deploy-frontend` depends on `frontend`, `nestjs-api`, and `ai-service`, so a frontend deployment cannot occur if any build or test fails. Workflow artifacts are scoped to their run and must use the same upload/download name.

## Branch, pull request, and deployment policy

| File | Trigger | Responsibility |
| --- | --- | --- |
| `ci.yml` | Any non-`main`/`master` push; pull request into `main`/`master` | Calls all three reusable validation workflows. |
| `preview-deploy.yml` | Any non-`main`/`master` push | Builds/tests the frontend, publishes `frontend-dist`, and calls `PREVIEW_DEPLOY_HOOK` when configured. |
| `deploy-production.yml` | Push to `main`/`master` after merge | Validates all layers, deploys GitHub Pages, and calls `BACKEND_DEPLOY_HOOK` when configured. |
| `reusable-frontend.yml` | `workflow_call` | `npm ci`, Vite build, Jest tests, upload `frontend-dist`. |
| `reusable-nest.yml` | `workflow_call` | `npm ci`, NestJS compilation, Jest tests. |
| `reusable-ai.yml` | `workflow_call` | Install Python dependencies and run `pytest ../../tests/ai_service -q`. |

YAML alone cannot prevent direct pushes to `main` or `master`. Apply a GitHub Ruleset or branch-protection rule as described in [.github/branch-protection.md](.github/branch-protection.md): require pull requests and approval, require `frontend-build`, `nest-build`, and `ai-test`, require an up-to-date branch, block force pushes, and restrict direct pushes.

GitHub Pages has a single production deployment, so feature branches use the `frontend-dist` artifact and may publish a separate preview through `PREVIEW_DEPLOY_HOOK`. Likewise, configure `BACKEND_DEPLOY_HOOK` or a hosting provider's Git integration to deploy NestJS/FastAPI after merge.

The production workflow runs all checks when `VITE_API_BASE_URL` is absent, but skips GitHub Pages deployment. Set that repository variable to the public HTTPS NestJS URL ending in `/api` to enable frontend deployment; Vite injects it at build time.

## Reusable workflow pattern

A reusable workflow declares `on: workflow_call`:

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

A caller job uses `uses`, not `runs-on`/`steps`:

```yaml
jobs:
  example:
    uses: ./.github/workflows/reusable-example.yml
```

This centralizes Node/Python setup and caching, keeps job names stable for branch protection, and ensures production, pull-request, and preview pipelines use the same commands. Pass only the secrets a reusable workflow needs; do not default to `secrets: inherit`.

## Common failures

| Problem | Cause / resolution |
| --- | --- |
| `mapping values are not allowed` | Check indentation, remove tabs, and quote a string containing `:` where appropriate. |
| Workflow does not appear | Ensure the file is in `.github/workflows/*.yml`, the trigger is correct, and YAML is valid. |
| Deployment runs before tests | Add the appropriate `needs` and `if: success()`. |
| Required check remains pending | The workflow may be skipped by `paths`, a branch filter, or `[skip ci]`; avoid those exclusions for required checks. |
| `npm ci` fails | Keep `package.json` and the lockfile synchronized. |
| Jest cannot find tests | Verify each package's Jest configuration still points to the shared `tests/frontend` or `tests/nestjs` directory. |
| Artifact is missing | Match upload/download names and ensure the upload runs after a successful build. |
| Secret is empty in a fork pull request | GitHub does not expose repository secrets to fork PRs; tests must not need them and deployment runs after merge/internal pushes. |
| `403 Resource not accessible` | Grant the smallest required permission scope. |
| Pages deployment is overwritten | Do not deploy GitHub Pages from every branch; use a preview artifact/host instead. |
| Direct push still succeeds | A Ruleset/branch-protection rule is missing or an administrator can bypass it. |

Official references: [workflow syntax](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax), [reusable workflows](https://docs.github.com/en/actions/how-tos/reuse-automations/reuse-workflows), and [protected branches/rulesets](https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-protected-branches/about-protected-branches).
