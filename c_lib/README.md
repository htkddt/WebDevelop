# M4-Hardcore AI Engine (C Library)

High-performance, minimalist C library for handling **1 billion records** with AI-driven insights. Built for **Apple Silicon M4** and **RTX 4070** heterogeneous environments.

**Consumer repos (no root build):** If you use **c_ai** or **python_ai** as separate repositories, follow the guidelines in [docs/consumers.md](docs/consumers.md) and, for Python, [docs/getting-started-python.md](docs/getting-started-python.md). Those docs describe how to get the library (build from c-lib or download prebuilt) and how to point c_ai / python_ai at it so they can run.

## Tech stack

| Layer      | Technology |
|-----------|------------|
| Core      | Pure C (Clang / Apple Silicon optimized) |
| Database  | MongoDB v8.0 (v2.x C Driver) — Vector Search |
| Cache     | Redis (Hiredis) — real-time counters |
| Analytics | Elasticsearch (ELK) — auto-language ingest pipeline |
| UI        | Terminal (Ncurses) + pipe-based debug monitor |

## Architecture

- **Multi-tenant:** Data isolation via strict `tenant_id` indexing.
- **Load balancer:** Task dispatching between M4 NPU and remote CUDA nodes.
- **Self-healing:** AI-driven auto-fix via `.patch` generation and re-compilation (optional).

## Prerequisites (cross-platform)

Required to build and run the engine:

| Dependency | Purpose | Required |
|------------|---------|----------|
| **C compiler** (Clang or GCC) | Build | Yes |
| **Ncurses** | Terminal UI | Yes |
| **libcurl** | Ollama HTTP API | Yes |
| **MongoDB C Driver** (libmongoc) | Chat persistence | No (optional) |
| **Ollama** | Local AI model (e.g. llama3) | Yes (for AI replies) |

### macOS (Apple Silicon / Intel)

```bash
# Xcode Command Line Tools (includes Clang) — install once
xcode-select --install

# Homebrew: https://brew.sh
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Required
brew install ncurses curl

# Optional: MongoDB chat persistence
brew install mongo-c-driver
```

- **Clang:** [Xcode Command Line Tools](https://developer.apple.com/xcode/resources/) or full Xcode  
- **Ncurses:** [Homebrew ncurses](https://formulae.brew.sh/formula/ncurses)  
- **libcurl:** [Homebrew curl](https://formulae.brew.sh/formula/curl) (system curl is often sufficient)  
- **MongoDB C Driver:** [mongo-c-driver](https://www.mongodb.com/docs/drivers/c/) · [Homebrew](https://formulae.brew.sh/formula/mongo-c-driver)  
- **Ollama:** [ollama.com](https://ollama.com) → download and run `ollama run llama3` (or another model)

### Linux (Debian / Ubuntu)

```bash
# Required
sudo apt update
sudo apt install -y build-essential libncurses-dev libcurl4-openssl-dev

# Optional: MongoDB chat persistence
# See: https://www.mongodb.com/docs/drivers/c/current/installation/
# Or use package if available, e.g. libmongoc-dev
```

- **Build-essential:** [GCC/Clang, make](https://packages.ubuntu.com/search?keywords=build-essential)  
- **Ncurses:** [libncurses-dev](https://packages.ubuntu.com/search?keywords=libncurses-dev)  
- **libcurl:** [libcurl4-openssl-dev](https://packages.ubuntu.com/search?keywords=libcurl4-openssl-dev)  
- **MongoDB C Driver:** [Install guide (Linux)](https://www.mongodb.com/docs/drivers/c/current/installation/)  
- **Ollama:** [ollama.com](https://ollama.com) or `curl -fsSL https://ollama.com/install.sh | sh`

### Linux (Fedora / RHEL)

```bash
# Required
sudo dnf install -y gcc clang make ncurses-devel libcurl-devel

# Optional: libmongoc (package name may vary)
# sudo dnf install -y mongo-c-driver-devel
```

### Windows

- **Compiler:** [Visual Studio Build Tools](https://visualstudio.microsoft.com/visual-cpp-build-tools/) (MSVC) or [Clang for Windows](https://releases.llvm.org/download.html) or [MinGW-w64](https://www.mingw-w64.org/)  
- **Ncurses:** [PDCurses](https://pdcurses.org/) or [vcpkg](https://vcpkg.io): `vcpkg install pdcurses`  
- **libcurl:** [curl.se Windows](https://curl.se/windows/) or vcpkg: `vcpkg install curl`  
- **MongoDB C Driver:** [Install on Windows](https://www.mongodb.com/docs/drivers/c/current/installation/)  
- **Ollama:** [ollama.com](https://ollama.com) — Windows installer  

The project Makefile is tuned for macOS/Clang; on Windows use CMake or adapt the Makefile (e.g. MinGW + MSYS2 with `pacman -S mingw-w64-ucrt-x86_64-ncurses mingw-w64-ucrt-x86_64-curl`).

## Quick start (M4)

```bash
# Validate environment
make validate

# Build
make all

# Run AI terminal (hybrid mode)
./bin/ai_bot --mode hybrid
```

Modes: `--mode m4` | `--mode cuda` | `--mode hybrid`. In the terminal, press **s** to simulate a batch, **q** to quit.

**Test the C library from C or Python:** build the lib (`make lib`), then run the **c_ai** C app (`make -C c_ai && ./c_ai/c_ai_bot "hello"`) or the **python_ai** script (`python3 python_ai/run_ai.py "hello"`), or the **HTTP server** (`python3 python_ai/server/app.py` — see [docs/api.md](docs/api.md#python-http-server-reference-consumer)). See `c_ai/README.md` and `python_ai/README.md`.

## Project layout

```
c-lib/               # C library only (no executable; can be its own repo)
  include/, src/    # Public API and implementation
  Makefile          # Builds lib/libm4engine.a and .so/.dylib only (make lib); no run target
  .cursor/rule.md   # C-library Cursor rules
  # Consumed as prebuilt .a/.so or .o by c_ai and python_ai (inject/link to execute)
c_ai/                # C test app — uses c-lib (make lib && make -C c_ai)
python_ai/           # Python test app — uses c-lib via ctypes (make lib && python3 python_ai/run_ai.py)
bin/                 # ai_bot binary (after make all)
lib/                 # Built artifacts: libm4engine.a, libm4engine.dylib / .so (after make lib)
build/               # Object files (make all / make lib)
```

## MongoDB database and collection

When wired (libmongoc), the engine uses:

| Setting    | Constant                    | Default   |
|-----------|-----------------------------|-----------|
| Database  | `STORAGE_MONGO_DB_NAME`     | `m4_ai`   |
| Collection| `STORAGE_MONGO_COLLECTION`  | `records` |

Defined in `c-lib/include/storage.h`. Every document must include `tenant_id` for multi-tenant isolation. To use a different db/collection, change those macros or add config in `engine_config_t`.

## MongoDB chat persistence (bot.records)

User and bot messages are written to MongoDB **database `bot`, collection `records`** when the driver is linked.

**Build with MongoDB (libmongoc):**
```bash
brew install mongo-c-driver
make USE_MONGOC=1
./bin/ai_bot --mode hybrid
```

Each document has: `tenant_id`, `role` (user/bot), `content`, `ts` (timestamp string). Without libmongoc (`make` only), chat still works but messages are not saved; you'll see a stub log line instead.

## Optional: Redis, Elasticsearch

Edit `Makefile` for Redis/ELK. The engine runs without them using stub implementations.

## Tutorial: Use the C library from other languages

You can compile the engine as a **shared library** and call it from Python 2/3, Java, C/C++, JavaScript/TypeScript, Go, Rust, Ruby, PHP, Swift, Kotlin, and others.

1. **Build the shared library**
   ```bash
   make lib
   ```
   Produces `lib/libm4engine.dylib` (macOS) or `lib/libm4engine.so` (Linux). Optional: `make lib USE_MONGOC=1`.

2. **Tutorial and examples**
   See **[docs/TUTORIAL_BINDINGS.md](docs/TUTORIAL_BINDINGS.md)** for:
   - **C/C++** — include headers, link with `-Llib -lm4engine`
   - **Python 2/3** — `ctypes` or `cffi` to load the `.so`/`.dylib` and call the C API
   - **Java** — JNA or JNI to load the library and call e.g. `ollama_query`, `engine_create`
   - **JavaScript/TypeScript (Node.js)** — `ffi-napi` or N-API native addon
   - **Go, Rust, Ruby, PHP, Swift, Kotlin** — short summary and links to each language’s FFI docs

   The tutorial lists the public C API (engine, ollama, storage, tenant, validate, **smart_topic**) and points to official install/docs for each toolchain. For **intent-based temperature** (TECH/CHAT/DEFAULT) and the mini AI switch, see **[docs/smart_topic.md](docs/smart_topic.md)**.

## Private registry: ship .a / .so / .o builds

To **publish pre-built artifacts** (static lib, shared lib, optional object files) to a private registry:

1. **Build a versioned package**
   ```bash
   make package                    # dist/m4engine-1.0.0-darwin-arm64.tar.gz
   make package USE_MONGOC=1       # with MongoDB
   make package INCLUDE_OBJ=1      # include build/*.o in tarball
   ```
2. **Upload** the tarball to your artifact server (Artifactory, Nexus, S3, GitHub Releases, or a private Docker image).  
   See **[docs/PRIVATE_REGISTRY.md](docs/PRIVATE_REGISTRY.md)** for curl/aws/gh/Docker examples and consumer usage.

3. **Free option (no paid registry):** Use **GitHub Actions** to build and publish to **GitHub Releases**.  
   - Workflow: [`.github/workflows/package.yml`](.github/workflows/package.yml) — on push to `main` it uploads an artifact; on push of a tag `v*` it creates a release and attaches the tarball.  
   - Publish: `git tag v1.0.0 && git push origin v1.0.0`.  
   - Download: `https://github.com/OWNER/REPO/releases/download/v1.0.0/m4engine-1.0.0-linux-amd64.tar.gz`.  
   Details: [docs/PRIVATE_REGISTRY.md § Free option](docs/PRIVATE_REGISTRY.md#free-option-github-actions--releases-no-paid-registry).
