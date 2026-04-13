# Project Setup Guide

---

# Frontend

## Navigate to project folder

```bash
cd fe
```

## Initialize project (Vite)

(Vite 2.9.0 requires Node v14)

* Template:

```bash
npm init vite@latest . -- --template react
# Specific version
npm init vite@2.9.0 . -- --template react
```

* Default:

```bash
npm create vite@latest
# Specific version
npm create vite@2.9.0
```

* Options:

  * Name
  * Framework: React
  * Variant: JavaScript + SWC

* Output:

  * ./src
  * .gitignore
  * index.html
  * package.json
  * vite.config.js

---

## Generate package.json (if not exists)

```bash
npm init -y   # Auto accept defaults
npm init      # Manual setup
```

---

## Install dependencies

```bash
npm install
```

* Clean install:

```bash
npm ci
```

* Install specific packages:

```bash
npm install react-router-dom@6.3.0 lucide-react@0.263.1
```

---

## Tailwind CSS setup

### Latest (Node v16+)

```bash
npm install -D tailwindcss@latest postcss@latest autoprefixer@latest
```

### Legacy (Node v14)

```bash
npm install -D tailwindcss@2.2.19 postcss@8.3.11 autoprefixer@10.4.0
```

### Generate config

```bash
npx tailwindcss init -p
```

---

## Maintenance

* Check unused packages:

```bash
npx depcheck
```

* Remove unused:

```bash
npm uninstall <package>
```

---

## Run frontend without Node

1. Download build artifact from GitHub Actions
2. Extract and copy to:

```
{project_path}/frontend/
```

3. Run:

```bash
python -m http.server 3000
```

4. Open:

```
http://localhost:3000
```

---

# Backend

## Setup environment

```bash
cd <backend_folder>/
make clean
make setup
```

* Applies to Java / NestJS

---

## Python backend

```bash
cd python_ai/server/
pip install -r requirements.txt
python app.py
```

---

## Node.js backend

```bash
cd backend/
npm run dev
```

* Requires server for handling requests

---

# Deployment

## Workflow

* Build:

```bash
git push origin master
```

* Release:

```bash
git tag v1.0.0
git push origin v1.0.0
```

---

## Frontend (GitHub Pages)

### Setup

* Settings → Public repo
* Settings → Pages → GitHub Actions

### Configuration

* Use `<HashRouter>` instead of `<Router>`

* vite.config.js:

```js
base: '/WebDevelop/'
```

* package.json:

```json
"homepage": "https://htkddt.github.io/WebDevelop/"
```

* URL format:

```
https://.../repo_name/#/...
```

* Handle backend failure with mock data

---

## Backend

* Deploy using Render

---

## Database

* Use MongoDB Atlas

---

## Gemini API Key

1. [https://aistudio.google.com](https://aistudio.google.com)
2. Get API key
3. Create API key
4. Use default project
