# Tutorial: Using the C library from other languages

## 1. Install

Download the prebuilt library from GitHub Releases. No cloning or compiling needed.

### Authentication (private repo)

This is a private repository. Authenticate with `gh` CLI first (one time):

```bash
brew install gh            # macOS
# or: sudo apt install gh  # Ubuntu
gh auth login              # follow prompts to authenticate
```

### Download + setup

```bash
# macOS (Apple Silicon)
gh release download --repo ngoky/lib_c --pattern "*darwin-arm64*"
tar xzf m4engine-*.tar.gz && rm m4engine-*.tar.gz

# Linux (amd64)
gh release download --repo ngoky/lib_c --pattern "*linux-amd64*"
tar xzf m4engine-*.tar.gz && rm m4engine-*.tar.gz

# Linux (arm64)
gh release download --repo ngoky/lib_c --pattern "*linux-arm64*"
tar xzf m4engine-*.tar.gz && rm m4engine-*.tar.gz

# Specific version (instead of latest)
gh release download v1.0.0-beta.3 --repo ngoky/lib_c --pattern "*darwin-arm64*"
```

Contents:
```
m4engine-*/
  include/    # C headers (api.h, etc.)
  lib/        # libm4engine.dylib or .so + .a
  VERSION     # e.g. 1.0.0-beta.3
  BUILD_INFO  # OS, ARCH, USE_MONGOC
```

### Per-language install

**Python** — add to `requirements.txt` (replace TAG with latest version):
```
m4engine @ https://github.com/ngoky/lib_c/releases/download/v1.0.0-beta.3/m4engine-1.0.0b3-py3-none-macosx_14_0_arm64.whl
```
```bash
pip install -r requirements.txt
# or direct (latest):
gh release download --repo ngoky/lib_c --pattern "*.whl" --pattern "*$(uname -m)*"
pip install m4engine-*.whl
```

Usage:
```python
from m4engine import Engine

with Engine({"mode": 2, "debug_modules": ["ai_agent"]}) as e:
    print(e.chat("default", "user_1", "Hello!"))

    # Streaming
    e.chat_stream("default", "user_1", "Tell me about Saigon.",
                  on_token=lambda text, done: print(text, end="" if not done else "\n"))
```

**Node.js / NestJS** — install from release:
```bash
# Download + install
gh release download --repo ngoky/lib_c --pattern "*.tgz" --pattern "*$(uname -m | sed 's/x86_64/amd64/')*"
npm install m4engine-*.tgz
```

Or in `package.json` (pin version):
```json
{
  "dependencies": {
    "m4engine": "https://github.com/ngoky/lib_c/releases/download/v1.0.0-beta.3/m4engine-1.0.0-beta.3-darwin-arm64.tgz"
  }
}
```

Usage:
```javascript
const { Engine } = require('m4engine');
const engine = new Engine({ mode: 2, debug_modules: ['ai_agent'] });
console.log(engine.chat('default', 'user_1', 'Hello!'));
engine.close();
```

**Java** — JNA + tarball:
```xml
<dependency>
  <groupId>com.sun.jna</groupId>
  <artifactId>jna</artifactId>
  <version>5.14.0</version>
</dependency>
```
```bash
gh release download --repo ngoky/lib_c --pattern "*.tar.gz" --pattern "*$(uname -m | sed 's/x86_64/amd64/')*"
tar xzf m4engine-*.tar.gz
# Run with: java -Djava.library.path=m4engine-*/lib ...
```

**Go** — tarball + cgo:
```bash
gh release download --repo ngoky/lib_c --pattern "*.tar.gz" --pattern "*$(uname -m | sed 's/x86_64/amd64/')*"
tar xzf m4engine-*.tar.gz -C ./m4engine
```
```go
// #cgo LDFLAGS: -L${SRCDIR}/m4engine/lib -lm4engine
```

### Verify

```bash
# Python
python3 -c "from m4engine import Engine; e=Engine({}); print('OK'); e.close()"

# Node
node -e "const {Engine}=require('m4engine'); const e=new Engine({}); console.log('OK'); e.close()"

# Any language — check binary
nm -g m4engine-*/lib/libm4engine.* 2>/dev/null | grep api_create
```

### Upgrade to latest

```bash
# Python
pip install --upgrade m4engine-*.whl

# Node
npm update m4engine

# Others — re-download latest tarball
gh release download --repo ngoky/lib_c --pattern "*.tar.gz" --clobber
```

### Optional services

```bash
# Ollama (local LLM — skip if using cloud API keys only)
curl -fsSL https://ollama.com/install.sh | sh && ollama pull llama3.2:1b

# MongoDB (required for modes 1/2/3)
docker run -d -p 27017:27017 mongo

# Redis (required for modes 2/3)
docker run -d -p 6379:6379 redis
```

### Build from source (optional)

Only needed if you want to modify the library or build with custom flags.

```bash
git clone <c-lib-repo-url> c-lib && cd c-lib
make lib USE_MONGOC=1    # shared library
make lib-static           # static library
make package              # tarball
```

---

## 2. Prerequisites for using the public API

| Requirement | Detail |
|-------------|--------|
| **Library** | `libm4engine.dylib` / `.so` from the release tarball |
| **Ollama** | Running on `127.0.0.1:11434` for local LLM, or set cloud API keys (`GROQ_API_KEY`, etc.) |
| **MongoDB** | Optional — required for modes 1/2/3 |
| **Redis** | Optional — required for modes 2/3 |

### Public API (7 functions)

```c
// Lifecycle — JSON options (recommended for all FFI)
api_context_t *api_create(const char *json_opts);          // NULL or "{}" = defaults
api_context_t *api_create_with_opts(const api_options_t *opts);  // C struct (legacy)
void            api_destroy(api_context_t *ctx);

// Chat (sync or stream)
int api_chat(api_context_t *ctx,
             const char *tenant_id, const char *user_id,
             const char *user_message,
             char *bot_reply_out, size_t out_size,
             api_stream_token_cb stream_cb, void *stream_userdata);

// History
int api_load_chat_history(api_context_t *ctx, const char *tenant_id, const char *user_id);
int api_get_history_message(api_context_t *ctx, int index, ...);

// Stats
void api_get_stats(api_context_t *ctx, api_stats_t *out);

// Geo data import
int api_geo_atlas_import_row(api_context_t *ctx, ...);
```

Full reference: [api.md](api.md).

### Options — JSON format

`api_create` accepts a JSON string. Any key not present uses default. No struct alignment needed.

**Example (full):**

```json
{
  "mode": 3,
  "mongo_uri": "mongodb://127.0.0.1:27017",
  "redis_host": "127.0.0.1",
  "redis_port": 6379,
  "es_host": "127.0.0.1",
  "es_port": 9200,
  "log_db": "my_logs",
  "log_coll": "chat_logs",
  "context_batch_size": 30,
  "inject_geo_knowledge": 0,
  "disable_auto_system_time": 0,
  "geo_authority": 1,
  "geo_authority_csv_path": "/data/provinces.csv",
  "geo_migrate_legacy": 0,
  "vector_gen_backend": 0,
  "vector_ollama_model": null,
  "embed_migration_autostart": 0,
  "session_idle_seconds": 300,
  "shared_collection_mongo_uri": null,
  "shared_collection_json_path": null,
  "shared_collection_backfill_db": null,
  "learning_terms_path": "/data/nl_terms.json",
  "enable_learning_terms": 1,
  "defer_learning_terms_load": 1,
  "default_persona": "You are a helpful assistant.",
  "default_instructions": "Always reply in Vietnamese.",
  "default_model_lane": 4,
  "debug_modules": ["API", "ai_agent", "STORAGE"],
  "lanes": [
    {"key": "BUSINESS", "model": "finance-llm", "api_url": "https://api.groq.com/openai/v1/chat/completions", "api_key": "gsk_..."},
    {"key": "TECH", "model": "codellama", "inject": "You are a code expert."}
  ]
}
```

**Example (minimal — everything defaults):**

```json
{}
```

**Example (just mode + debug):**

```json
{"mode": 2, "debug_modules": ["ai_agent"]}
```

### Options reference

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `mode` | int | `1` | `0`=memory, `1`=mongo, `2`=mongo+redis, `3`=mongo+redis+elk |
| `mongo_uri` | string | `"mongodb://127.0.0.1:27017"` | MongoDB URI |
| `redis_host` | string | `"127.0.0.1"` | Redis host |
| `redis_port` | int | `6379` | Redis port |
| `es_host` | string | null | Elasticsearch host (null=disabled) |
| `es_port` | int | `9200` | Elasticsearch port |
| `log_db` / `log_coll` | string | null | Override ai_logs DB/collection |
| `context_batch_size` | int | `30` | History cycles for LLM context |
| `inject_geo_knowledge` | int | `0` | `1`=prepend [KNOWLEDGE_BASE] |
| `disable_auto_system_time` | int | `0` | `1`=skip auto [SYSTEM_TIME] |
| `geo_authority` | int | `0` | `1`=enable L1 cache + conflict detector |
| `geo_authority_csv_path` | string | null | CSV file loaded at init |
| `geo_migrate_legacy` | int | `0` | `1`=auto-run geo_atlas backfill |
| `vector_gen_backend` | int | `0` | `0`=built-in hash, `1`=external embed model |
| `vector_ollama_model` | string | null | Override embed model when backend=1 |
| `embed_migration_autostart` | int | `0` | `1`=queue embed migration at init |
| `session_idle_seconds` | int | `300` | Idle eviction seconds |
| `shared_collection_*` | string | null | SharedCollection config (see api.md) |
| `learning_terms_path` | string | null | NL routing terms file |
| `enable_learning_terms` | int | `0` | `1`=allow cue recording |
| `defer_learning_terms_load` | int | `0` | `1`=background thread load |
| `default_persona` | string | null | Persona (null=compiled-in default) |
| `default_instructions` | string | null | Extra instructions |
| `default_model_lane` | int | `0` | `0`=DEFAULT, `1`=EDUCATION, `2`=BUSINESS, `3`=TECH, `4`=CHAT |
| `debug_modules` | string[] | null | Module keys for DEBUG logging |
| `lanes` | object[] | null | Model lanes with optional direct endpoints |

### Lanes

```json
{"key": "BUSINESS", "model": "finance-llm", "api_url": "https://...", "api_key": "gsk_...", "inject": "..."}
```

| Field | Required | Description |
|-------|----------|-------------|
| `key` | yes | Lane name |
| `model` | no | Model ID |
| `inject` | no | System inject text |
| `api_url` | no | Direct endpoint (null=cloud pool routing) |
| `api_key` | no | API key for direct endpoint |

### Debug modules

Valid keys: `API`, `ai_agent`, `STORAGE`, `GEO_LEARNING`, `GEO_AUTH`, `OLLAMA`, `ELK`, `EMBED_MIGRATION`, `ENGINE`, `CHAT`, `nl_learn_terms`, `LOGIC_CONFLICT`

Also via env: `M4_DEBUG_MODULES=API,ai_agent`

```
[ai_agent][DEBUG] complete_chat: model_pin=... lane_url=...   ← only if ai_agent in list
[ai_agent][INFO] tier 1: trying gemini model=gemini-2.0-flash ← always logged
[ai_agent][WARN] cloud pool: ALL tiers failed                 ← always logged
[ai_agent][ERROR] Ollama fallback FAILED rc=-1                ← always logged
```

### `api_stats_t` — output fields

```json
{
  "memory_bytes":      { "type": "uint64", "note": "Estimated session ring buffer footprint" },
  "mongo_connected":   { "type": "int",    "values": { "0": "down", "1": "up" } },
  "redis_connected":   { "type": "int",    "values": { "0": "down", "1": "up" } },
  "elk_enabled":       { "type": "int",    "values": { "0": "off",  "1": "configured" } },
  "elk_connected":     { "type": "int",    "values": { "0": "unreachable", "1": "reachable" } },
  "ollama_connected":  { "type": "int",    "values": { "0": "down", "1": "running" } },
  "error_count":       { "type": "uint64", "note": "Total errors (monotonic)" },
  "warning_count":     { "type": "uint64", "note": "Total warnings (monotonic)" },
  "processed":         { "type": "uint64", "note": "Successfully processed turns" },
  "errors":            { "type": "uint64", "note": "Engine-level errors" },
  "mongoc_linked":     { "type": "int",    "values": { "0": "stub (no USE_MONGOC)", "1": "real driver linked" } },
  "last_reply_source": {
    "type": "char",
    "values": {
      "0":   "no reply yet",
      "'M'": "API_SOURCE_MEMORY — current session",
      "'R'": "API_SOURCE_REDIS — vector cache hit",
      "'G'": "API_SOURCE_MONGODB — loaded from history",
      "'O'": "API_SOURCE_OLLAMA — local Ollama",
      "'C'": "API_SOURCE_CLOUD — hosted LLM (Groq/Cerebras/Gemini)"
    }
  },
  "last_chat_wire": {
    "type": "unsigned",
    "values": {
      "0": "API_CHAT_WIRE_NONE",
      "1": "API_CHAT_WIRE_OPENAI_CHAT — Groq, Cerebras",
      "2": "API_CHAT_WIRE_GEMINI",
      "3": "API_CHAT_WIRE_OLLAMA",
      "4": "API_CHAT_WIRE_REDIS_RAG — cache hit, no LLM",
      "5": "API_CHAT_WIRE_EXTERNAL — host-supplied text"
    }
  },
  "last_llm_model": {
    "type": "char[160]",
    "format": "provider:model_id",
    "examples": ["groq:llama-3.1-8b-instant", "ollama:llama3.2:1b", "redis_rag", ""]
  }
}
```

---

## 3. C / C++

### Compile and link

```bash
# C
clang -std=c17 -Iinclude -Llib -lm4engine -lm -lpthread -lcurl \
      -Wl,-rpath,lib -o myapp myapp.c

# C++
clang++ -std=c++17 -Iinclude -Llib -lm4engine -lm -lpthread -lcurl \
        -Wl,-rpath,lib -o myapp myapp.cpp
```

### Example: sync chat

```c
#include "api.h"
#include <stdio.h>

int main(void) {
    api_context_t *ctx = api_create(
        "{\"mode\": 0, \"default_persona\": \"You are a helpful assistant.\"}"
    );
    if (!ctx) return 1;

    char reply[32768];
    if (api_chat(ctx, "default", "user_1", "Hello!",
                 reply, sizeof(reply), NULL, NULL) == 0)
        printf("Bot: %s\n", reply);

    api_destroy(ctx);
    return 0;
}
```

### Example: streaming chat

```c
#include "api.h"
#include <stdio.h>

void on_token(const char *token, const char *msg_id, int done, void *ud) {
    (void)msg_id; (void)ud;
    if (!done) printf("%s", token);
    else       printf("\n");
}

int main(void) {
    api_context_t *ctx = api_create("{\"mode\": 0, \"debug_modules\": [\"ai_agent\"]}");
    if (!ctx) return 1;

    char reply[32768];
    api_chat(ctx, "default", "user_1", "Tell me about Vietnam.",
             reply, sizeof(reply), on_token, NULL);

    api_destroy(ctx);
    return 0;
}
```

### Example: history + stats

```c
api_load_chat_history(ctx, "default", "user_1");

char role[32], content[4096], ts[24]; char src;
for (int i = 0; ; i++) {
    if (api_get_history_message(ctx, i,
            role, sizeof(role), content, sizeof(content),
            &src, ts, sizeof(ts), NULL, 0) != 0)
        break;
    printf("[%c] %s: %s\n", src, role, content);
}

api_stats_t st;
api_get_stats(ctx, &st);
printf("Processed: %llu, Mongo: %s, Last source: %c\n",
       st.processed, st.mongo_connected ? "up" : "down", st.last_reply_source);
```

---

## 4. Python (ctypes)

### Minimal sync chat

```python
import ctypes, json

lib = ctypes.CDLL("lib/libm4engine.dylib")  # or .so on Linux

# Setup signatures — just strings, no struct alignment needed
lib.api_create.restype = ctypes.c_void_p
lib.api_create.argtypes = [ctypes.c_char_p]
lib.api_destroy.argtypes = [ctypes.c_void_p]
lib.api_chat.argtypes = [
    ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
    ctypes.c_char_p, ctypes.c_size_t,
    ctypes.c_void_p, ctypes.c_void_p
]
lib.api_chat.restype = ctypes.c_int

# Create with JSON — no struct needed
opts = json.dumps({
    "mode": 0,
    "default_persona": "You are a helpful assistant.",
    "debug_modules": ["ai_agent"]
})
ctx = lib.api_create(opts.encode())

reply = ctypes.create_string_buffer(32768)
rc = lib.api_chat(ctx, b"default", b"user_1", b"Hello!",
                  reply, 32768, None, None)
if rc == 0:
    print("Bot:", reply.value.decode())

lib.api_destroy(ctx)
```

### Streaming with callback

```python
StreamCB = ctypes.CFUNCTYPE(
    None, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_void_p
)

def on_token(token, msg_id, done, ud):
    if token and not done:
        print(token.decode(), end="", flush=True)
    if done:
        print()

cb = StreamCB(on_token)
reply = ctypes.create_string_buffer(32768)
lib.api_chat(ctx, b"default", b"user_1", b"Tell me about Saigon.",
             reply, 32768, cb, None)
```

### Stats

```python
class ApiStats(ctypes.Structure):
    _fields_ = [
        ("memory_bytes", ctypes.c_uint64),
        ("mongo_connected", ctypes.c_int),
        ("redis_connected", ctypes.c_int),
        ("elk_enabled", ctypes.c_int),
        ("elk_connected", ctypes.c_int),
        ("ollama_connected", ctypes.c_int),
        ("error_count", ctypes.c_uint64),
        ("warning_count", ctypes.c_uint64),
        ("processed", ctypes.c_uint64),
        ("errors", ctypes.c_uint64),
        ("mongoc_linked", ctypes.c_int),
        ("last_reply_source", ctypes.c_char),
        ("last_chat_wire", ctypes.c_uint),
        ("last_llm_model", ctypes.c_char * 160),
    ]

lib.api_get_stats.argtypes = [ctypes.c_void_p, ctypes.POINTER(ApiStats)]
st = ApiStats()
lib.api_get_stats(ctx, ctypes.byref(st))
print(f"Processed: {st.processed}, Source: {st.last_reply_source.decode()}")
```

**Important:** `ApiOptions` struct must match `api.h` field order **exactly**. See `python_ai/engine_ctypes.py` for the canonical Python definition.

---

## 5. Java (JNA)

```java
import com.sun.jna.*;
import com.sun.jna.ptr.*;

public interface M4Engine extends Library {
    M4Engine LIB = Native.load("m4engine", M4Engine.class);

    Pointer api_create(String jsonOpts);  // JSON string or null for defaults
    void api_destroy(Pointer ctx);
    int api_chat(Pointer ctx, String tenant, String user, String message,
                 byte[] replyOut, int outSize,
                 Pointer streamCb, Pointer userdata);
}

// Usage:
Pointer ctx = M4Engine.LIB.api_create("{\"mode\": 0, \"debug_modules\": [\"ai_agent\"]}");
byte[] reply = new byte[32768];
int rc = M4Engine.LIB.api_chat(ctx, "default", "user_1", "Hello!",
                                reply, 32768, null, null);
if (rc == 0)
    System.out.println("Bot: " + new String(reply, "UTF-8").trim());
M4Engine.LIB.api_destroy(ctx);
```

Run with: `java -Djava.library.path=lib ...`

---

## 6. Node.js (ffi-napi)

```bash
npm install ffi-napi ref-napi
```

```javascript
const ffi = require('ffi-napi');
const ref = require('ref-napi');
const path = require('path');

const lib = ffi.Library(path.join(__dirname, '..', 'lib', 'libm4engine'), {
  api_create:  ['pointer', ['string']],
  api_destroy: ['void',    ['pointer']],
  api_chat:    ['int',     ['pointer', 'string', 'string', 'string',
                            'pointer', 'size_t', 'pointer', 'pointer']],
});

const opts = JSON.stringify({ mode: 0, debug_modules: ['ai_agent'] });
const ctx = lib.api_create(opts);
const reply = Buffer.alloc(32768);

const rc = lib.api_chat(ctx, 'default', 'user_1', 'Hello!',
                         reply, 32768, ref.NULL, ref.NULL);
if (rc === 0)
  console.log('Bot:', reply.toString('utf8').replace(/\0/g, ''));

lib.api_destroy(ctx);
```

---

## 7. Go (cgo)

```go
package main

/*
#cgo CFLAGS: -I${SRCDIR}/../include
#cgo LDFLAGS: -L${SRCDIR}/../lib -lm4engine -lm -lpthread -lcurl
#include "api.h"
#include <stdlib.h>
*/
import "C"
import "fmt"
import "unsafe"

func main() {
    opts := C.CString(`{"mode": 0, "debug_modules": ["ai_agent"]}`)
    defer C.free(unsafe.Pointer(opts))

    ctx := C.api_create(opts)
    if ctx == nil { panic("api_create failed") }
    defer C.api_destroy(ctx)

    tenant := C.CString("default")
    user   := C.CString("user_1")
    msg    := C.CString("Hello!")
    defer C.free(unsafe.Pointer(tenant))
    defer C.free(unsafe.Pointer(user))
    defer C.free(unsafe.Pointer(msg))

    reply := (*C.char)(C.malloc(32768))
    defer C.free(unsafe.Pointer(reply))

    rc := C.api_chat(ctx, tenant, user, msg, reply, 32768, nil, nil)
    if rc == 0 {
        fmt.Println("Bot:", C.GoString(reply))
    }
}
```

---

## 8. NestJS (TypeScript + ffi-napi)

### Setup

```bash
npm install ffi-napi ref-napi
```

### Create `m4engine.service.ts`

```typescript
import { Injectable, OnModuleInit, OnModuleDestroy } from '@nestjs/common';
import * as ffi from 'ffi-napi';
import * as ref from 'ref-napi';
import * as path from 'path';

@Injectable()
export class M4EngineService implements OnModuleInit, OnModuleDestroy {
  private lib: any;
  private ctx: any;

  onModuleInit() {
    const libPath = process.env.M4ENGINE_LIB
      || path.join(__dirname, '..', '..', 'c-lib', 'lib', 'libm4engine');

    this.lib = ffi.Library(libPath, {
      api_create:              ['pointer', ['string']],
      api_destroy:             ['void',    ['pointer']],
      api_chat:                ['int',     ['pointer', 'string', 'string', 'string',
                                            'pointer', 'size_t', 'pointer', 'pointer']],
      api_load_chat_history:   ['int',     ['pointer', 'string', 'string']],
      api_get_history_message: ['int',     ['pointer', 'int', 'pointer', 'size_t',
                                            'pointer', 'size_t', 'pointer', 'pointer',
                                            'size_t', 'pointer', 'size_t']],
      api_get_stats:           ['void',    ['pointer', 'pointer']],
    });

    const opts = JSON.stringify({
      mode: parseInt(process.env.M4ENGINE_MODE || '2'),
      mongo_uri: process.env.M4ENGINE_MONGO_URI || 'mongodb://127.0.0.1:27017',
      redis_host: process.env.M4ENGINE_REDIS_HOST || '127.0.0.1',
      default_persona: process.env.M4ENGINE_PERSONA || null,
      debug_modules: (process.env.M4_DEBUG_MODULES || '').split(',').filter(Boolean),
      lanes: JSON.parse(process.env.M4ENGINE_LANES || 'null'),
    });

    this.ctx = this.lib.api_create(opts);
    if (this.ctx.isNull()) {
      throw new Error('M4Engine: api_create failed');
    }
  }

  onModuleDestroy() {
    if (this.ctx && !this.ctx.isNull()) {
      this.lib.api_destroy(this.ctx);
    }
  }

  chat(tenantId: string, userId: string, message: string): string | null {
    const reply = Buffer.alloc(32768);
    const rc = this.lib.api_chat(
      this.ctx, tenantId, userId, message,
      reply, 32768, ref.NULL, ref.NULL,
    );
    if (rc !== 0) return null;
    return reply.toString('utf8').replace(/\0/g, '').trim();
  }

  loadHistory(tenantId: string, userId: string): number {
    return this.lib.api_load_chat_history(this.ctx, tenantId, userId);
  }

  getHistoryMessages(tenantId: string, userId: string): Array<{role: string; content: string; source: string}> {
    this.loadHistory(tenantId, userId);
    const messages: Array<{role: string; content: string; source: string}> = [];
    const roleBuf = Buffer.alloc(32);
    const contentBuf = Buffer.alloc(8192);
    const sourceBuf = Buffer.alloc(1);

    for (let i = 0; ; i++) {
      const rc = this.lib.api_get_history_message(
        this.ctx, i,
        roleBuf, 32, contentBuf, 8192,
        sourceBuf, ref.NULL, 0, ref.NULL, 0,
      );
      if (rc !== 0) break;
      messages.push({
        role: roleBuf.toString('utf8').replace(/\0/g, ''),
        content: contentBuf.toString('utf8').replace(/\0/g, ''),
        source: sourceBuf.toString('utf8', 0, 1),
      });
    }
    return messages;
  }
}
```

### Create `chat.controller.ts`

```typescript
import { Controller, Post, Get, Body, Query } from '@nestjs/common';
import { M4EngineService } from './m4engine.service';

@Controller('api')
export class ChatController {
  constructor(private readonly engine: M4EngineService) {}

  @Post('chat')
  chat(@Body() body: { message: string; tenant_id?: string; user_id?: string }) {
    const reply = this.engine.chat(
      body.tenant_id || 'default',
      body.user_id || 'default',
      body.message,
    );
    return { reply, tenant_id: body.tenant_id || 'default' };
  }

  @Get('history')
  history(@Query('tenant_id') tenant?: string, @Query('user_id') user?: string) {
    return { messages: this.engine.getHistoryMessages(tenant || 'default', user || 'default') };
  }
}
```

### Register in `app.module.ts`

```typescript
import { Module } from '@nestjs/common';
import { M4EngineService } from './m4engine.service';
import { ChatController } from './chat.controller';

@Module({
  providers: [M4EngineService],
  controllers: [ChatController],
})
export class AppModule {}
```

### Environment

```bash
# .env
M4ENGINE_LIB=/path/to/c-lib/lib/libm4engine
M4ENGINE_MODE=2
M4ENGINE_MONGO_URI=mongodb://127.0.0.1:27017
M4ENGINE_PERSONA="You are a helpful assistant."
M4_DEBUG_MODULES=ai_agent,API
M4ENGINE_LANES=[{"key":"TECH","model":"codellama"}]
```

### Run

```bash
npm run start:dev
# POST http://localhost:3000/api/chat  {"message": "Hello!"}
# GET  http://localhost:3000/api/history?tenant_id=default&user_id=user_1
```

### Notes

- `M4EngineService` is a singleton — one context for the process, same as Python Flask
- For streaming: use `ffi-napi` callback + SSE (`@Sse()` decorator in NestJS) — same pattern as Python ctypes callback
- `api_get_stats` requires a ctypes-style struct buffer — or wrap in a helper that reads fields

---

## 9. Other languages

| Language | FFI mechanism | Link |
|----------|--------------|------|
| **Rust** | `libloading` crate or `extern "C"` + build.rs | [libloading](https://crates.io/crates/libloading) |
| **Ruby** | `ffi` gem: `attach_function :api_create, ...` | [Ruby FFI](https://github.com/ffi/ffi) |
| **PHP** | `FFI::cdef()` + `FFI::load()` (PHP 7.4+) | [PHP FFI](https://www.php.net/manual/en/book.ffi.php) |
| **Swift** | C bridging header, import module | [Swift C interop](https://www.swift.org/documentation/c-interop/) |
| **Kotlin/Native** | `cinterop` from C headers | [Kotlin Native](https://kotlinlang.org/docs/native-c-interop.html) |

All languages follow the same pattern:
1. Load `libm4engine.{dylib,so}`
2. Declare FFI signatures for the 7 public functions
3. Call `api_create` → `api_chat` → `api_destroy`

---

## 9. Flask + Vite integration (Python server)

### ctypes `ApiOptions` must match `api.h`

The struct `api_options_t` in `include/api.h` must be mirrored **field-by-field, in order** in `python_ai/engine_ctypes.py`. If Python is shorter, ctypes packs a wrong layout → segfault.

### Server initialization (`python_ai/server/app.py`)

| Step | What runs |
|------|-----------|
| 1 | `load_lib()` — resolve `libm4engine` via `M4ENGINE_LIB` or `../c-lib/lib/` |
| 2 | `build_api_options()` — populate `ApiOptions` from env |
| 3 | `api_create(&opts)` — single process-wide context |
| 4 | `api_load_chat_history` — prime in-memory history |
| 5 | `atexit → api_destroy` — clean shutdown |

### HTTP routes

| Route | c-lib function | Response |
|-------|----------------|----------|
| `POST /api/chat` | `api_chat(ctx, ..., NULL, NULL)` | JSON: `reply`, `tenant_id`, `user`, `source`, `llm_model` |
| `POST /api/chat/stream` | `api_chat(ctx, ..., cb, ud)` | SSE: `data: {"token","temp_message_id","done"}` per line |
| `GET /api/history` | `api_load_chat_history` + `api_get_history_message` loop | JSON: `messages[]` |
| `GET /api/stats` | `api_get_stats` | JSON: all `api_stats_t` fields |

### Vite frontend (`fe/`)

| Env | Purpose |
|-----|---------|
| `VITE_API_URL` | Flask origin (e.g. `http://127.0.0.1:5001`) |
| `VITE_TENANT_ID` | Default `"default"` |

Stream: POST to `/api/chat/stream`, read SSE `data:` lines, append `token` until `done: true`.

```
Vite (fe) ──POST /api/chat/stream──► Flask ──ctypes──► api_chat(cb) ──► Ollama/Cloud
                                         ◄──SSE tokens──
```

---

## 10. Publishing to a private registry

### Package tarball

```bash
make package                  # → dist/m4engine-<VER>-<OS>-<ARCH>.tar.gz
make package USE_MONGOC=1     # with MongoDB
```

### GitHub Releases (free, recommended)

Workflow: [`.github/workflows/package.yml`](../.github/workflows/package.yml)

- Push to `main` → auto-tag from `ENGINE_VERSION` in `include/engine.h` → GitHub Release
- Beta: set `ENGINE_VERSION` to `1.0.0-beta.1` → pre-release

### Other registries

| Registry | Upload | Consume |
|----------|--------|---------|
| **Artifactory / Nexus** | `curl -u USER:TOKEN -X PUT "URL" -T dist/*.tar.gz` | `curl -u USER:TOKEN -o m4.tar.gz "URL"` |
| **AWS S3** | `aws s3 cp dist/*.tar.gz s3://BUCKET/m4engine/${VER}/` | `aws s3 cp s3://... .` |
| **Docker** | Multi-stage build + push | `docker pull` + copy from image |

### Consumer usage

```bash
tar xzf m4engine-1.0.0-linux-amd64.tar.gz
gcc myapp.c -I m4engine-*/include -L m4engine-*/lib \
            -lm4engine -lm -lpthread -lcurl -o myapp
```
