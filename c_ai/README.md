# c_ai

C test app that links and uses the **c-lib** (M4-Hardcore AI Engine) as a reference consumer.

## Build

From repo root:

```bash
make lib          # build lib/libm4engine.dylib (or .so)
make -C c_ai      # build c_ai_bot
```

## Run

```bash
./c_ai/c_ai_bot "Your question"
# or from c_ai/:
cd c_ai && ./c_ai_bot "Hello"
```

Requires [Ollama](https://ollama.com) running (e.g. `ollama run llama3.2`) for `ollama_query`; the engine create/destroy test works without it.
