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

## Backend Deployment (Render)

### Connect to GitHub

* Go to [https://render.com](https://render.com)
* Click New + → Web Service
* Choose Build and deploy from a Git repository
* Connect and select the repository containing your backend code

### Configuration

* Name

```
<your-service-name>
```

* Root Directory (Repos name is default):

```
./backend/python_ai/server/
```

* Build Command:

```bash
pip install -r requirements.txt
```

* Start Command (The requirements.txt file must include "gunicorn"):

```bash
gunicorn <script_name>:app # Ex: python app.py -> gunicorn app:app
```

* Add Environment Variables (Advanced -> Add Environment Variable):

  * MONGODB_URL
  * GEMINI_API_KEY

* Modify Environment Variables (At left bar -> Environment)

  * MONGODB_URL
  * GEMINI_API_KEY

---

## Database Deployment (MongoDB Atlas)

### Create Cluster

* Go to [https://mongodb.com/cloud/atlas](https://mongodb.com/cloud/atlas)
* Login by Google account
* Choose Create a deployment -> Choose M0 Free tier
* Select provider (AWS recommended)
* Select region (Singapore recommended)
* Click Create

### Setup Access

* Create user name and password
* IP Access List -> Choose "Allow Access from Anywhere" or typing:

```
0.0.0.0/0
```

### Connection String

* At Database tab -> Connect
* Select drivers (Node.js / Python / ...)

```
mongodb+srv://<username>:<password>@cluster0.xxxxx.mongodb.net/?retryWrites=true&w=majority
```

* Change password
  
  * At left bar -> Database access
  * Edit -> Edit password -> Update user
  * Re-update Connection string

### How to push data directly to MongoDB Atlas?

```
python ./backend/python_ai/server/seed.py
```

---

## Gemini API Key

1. [https://aistudio.google.com](https://aistudio.google.com)
2. Get API key
3. Create API key
4. Use default project
