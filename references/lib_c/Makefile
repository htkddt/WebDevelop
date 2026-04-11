# M4-Hardcore AI Engine — C library (standalone repo, library-only)
# c-lib does NOT build or run an executable. It only produces the library (.a, .so/.dylib)
# for consumption by c_ai, python_ai, or the parent repo (which builds ai_bot from c-lib sources).

CC       = clang
CFLAGS   = -std=c17 -Wall -Wextra -O3
ifeq ($(shell uname -s),Darwin)
  CFLAGS += -arch arm64
endif
# glibc + -std=c17: strdup/strnlen live behind a feature-test macro (macOS libc does not need this).
ifeq ($(shell uname -s),Linux)
  CFLAGS += -D_DEFAULT_SOURCE
endif
LDFLAGS  = -lm -lncurses -lpthread -lcurl

USE_MONGOC   ?= 0
ifeq ($(USE_MONGOC),1)
  # Try pkg-config first (works on Linux + brew with pkg-config)
  MONGOC_PKG := $(shell pkg-config --cflags libmongoc-1.0 2>/dev/null)
  ifneq ($(MONGOC_PKG),)
    CFLAGS  += -DUSE_MONGOC $(shell pkg-config --cflags libmongoc-1.0)
    LDFLAGS += $(shell pkg-config --libs libmongoc-1.0)
  else
    # Fallback: brew prefix (macOS) or /usr (Linux)
    MONGODB_ROOT ?= $(shell brew --prefix mongo-c-driver 2>/dev/null || echo "/usr")
    MONGOC_INC   := $(wildcard $(MONGODB_ROOT)/include/mongoc-*) $(wildcard $(MONGODB_ROOT)/include/bson-*)
    ifeq ($(MONGOC_INC),)
      MONGOC_INC   := $(wildcard $(MONGODB_ROOT)/include/libmongoc-*) $(wildcard $(MONGODB_ROOT)/include/libbson-*)
      ifeq ($(MONGOC_INC),)
        MONGOC_INC := $(MONGODB_ROOT)/include/libmongoc-1.0 $(MONGODB_ROOT)/include/libbson-1.0
      endif
      MONGOC_LIBS  := -lmongoc-1.0 -lbson-1.0
    else
      MONGOC_LIBS  := -lmongoc2 -lbson2
    endif
    CFLAGS  += -DUSE_MONGOC $(addprefix -I,$(MONGOC_INC))
    LDFLAGS += -L$(MONGODB_ROOT)/lib $(MONGOC_LIBS)
  endif
endif

# Language detection: lang.c only (Unicode heuristic per .cursor/language.md; no CLD3, no Python).

INC_DIR   = include
SRC_DIR   = src
BUILD     = build
LIB_DIR   = lib
DIST_DIR  = dist

CONFIG_STAMP = $(BUILD)/.config_use_mongoc_$(USE_MONGOC)

# Library-only sources (no main.c — c-lib is not runnable)
LIB_SOURCES  = $(SRC_DIR)/engine.c $(SRC_DIR)/tenant.c $(SRC_DIR)/dispatcher.c \
               $(SRC_DIR)/storage.c $(SRC_DIR)/redis.c $(SRC_DIR)/validate.c $(SRC_DIR)/ollama.c \
               $(SRC_DIR)/ai_agent.c \
               $(SRC_DIR)/stat.c $(SRC_DIR)/api.c $(SRC_DIR)/smart_topic.c $(SRC_DIR)/model_switch.c \
               $(SRC_DIR)/geo_learning.c \
               $(SRC_DIR)/geo_authority.c $(SRC_DIR)/conflict_detector.c $(SRC_DIR)/utils.c $(SRC_DIR)/lang.c \
               $(SRC_DIR)/vector_generate.c $(SRC_DIR)/embed.c $(SRC_DIR)/embed_worker.c \
               $(SRC_DIR)/elk.c $(SRC_DIR)/shared_collection.c $(SRC_DIR)/elk_sync_pool.c \
               $(SRC_DIR)/nl_learn_terms.c $(SRC_DIR)/nl_learn_cues.c \
               $(SRC_DIR)/intent_route.c $(SRC_DIR)/intent_learn.c \
               $(SRC_DIR)/debug_log.c $(SRC_DIR)/json_opts.c
LIB_OBJECTS  = $(patsubst $(SRC_DIR)/%.c,$(BUILD)/lib_%.o,$(LIB_SOURCES))
UNAME_S      := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  LIB_NAME    = libm4engine.dylib
  LIB_LDFLAGS = -dynamiclib -install_name @rpath/$(LIB_NAME)
else
  LIB_NAME    = libm4engine.so
  LIB_LDFLAGS = -shared
endif

# Override from CI, e.g. tag v1.2.3 → make package VERSION=1.2.3 (keep in sync with include/engine.h)
VERSION   ?= $(shell grep -E 'ENGINE_VERSION' $(INC_DIR)/engine.h 2>/dev/null | sed 's/.*"\([^"]*\)".*/\1/' || echo "1.0.0")
UNAME_M   := $(shell uname -m)
OS        := $(shell echo "$(UNAME_S)" | tr '[:upper:]' '[:lower:]')
ARCH      := $(shell echo "$(UNAME_M)" | sed 's/x86_64/amd64/;s/aarch64/arm64/')
DIST_NAME = m4engine-$(VERSION)-$(OS)-$(ARCH)

AR      ?= ar
RANLIB  ?= ranlib

.PHONY: all clean validate lib lib-mongo lib-static lib-o package

all: lib

# Recommended for local dev when mongo-c-driver is installed: same linkage as python_ai / Flask
# (USE_MONGOC=1). Catches #ifdef USE_MONGOC paths; `make validate` still smoke-builds without Mongo for CI.
lib-mongo:
	$(MAKE) lib USE_MONGOC=1

$(CONFIG_STAMP):
	@mkdir -p $(BUILD)
	@rm -f $(BUILD)/.config_use_mongoc_0 $(BUILD)/.config_use_mongoc_1
	@touch $@

$(BUILD)/%.o: $(SRC_DIR)/%.c $(CONFIG_STAMP)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(INC_DIR) -c -o $@ $<

$(BUILD)/lib_%.o: $(SRC_DIR)/%.c $(CONFIG_STAMP)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fPIC -I$(INC_DIR) -c -o $@ $<

$(LIB_DIR)/$(LIB_NAME): $(LIB_OBJECTS)
	@mkdir -p $(LIB_DIR)
	$(CC) $(LIB_LDFLAGS) -o $@ $(LIB_OBJECTS) $(LDFLAGS)

$(LIB_DIR)/libm4engine.a: $(LIB_OBJECTS)
	@mkdir -p $(LIB_DIR)
	$(AR) rcs $@ $(LIB_OBJECTS)
	$(RANLIB) $@ 2>/dev/null || true

lib: $(LIB_DIR)/$(LIB_NAME)
lib-static: $(LIB_DIR)/libm4engine.a
lib-all: $(LIB_DIR)/$(LIB_NAME) $(LIB_DIR)/libm4engine.a

package: lib lib-static
	@mkdir -p $(DIST_DIR)/$(DIST_NAME)/include $(DIST_DIR)/$(DIST_NAME)/lib
	cp $(INC_DIR)/*.h $(DIST_DIR)/$(DIST_NAME)/include/
	cp $(LIB_DIR)/libm4engine.a $(LIB_DIR)/$(LIB_NAME) $(DIST_DIR)/$(DIST_NAME)/lib/
	@echo "$(VERSION)" > $(DIST_DIR)/$(DIST_NAME)/VERSION
	@echo "OS=$(OS) ARCH=$(ARCH) USE_MONGOC=$(USE_MONGOC)" > $(DIST_DIR)/$(DIST_NAME)/BUILD_INFO
	(cd $(DIST_DIR) && tar czf $(DIST_NAME).tar.gz $(DIST_NAME))
	@echo "Created $(DIST_DIR)/$(DIST_NAME).tar.gz"

validate:
	@echo "=== Validating (c-lib) ==="
	@(command -v $(CC) >/dev/null 2>&1 || command -v clang >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1 || command -v cc >/dev/null 2>&1) || \
		(echo "ERROR: No C compiler. Set CC= or install clang/gcc."; exit 1)
	@echo "  [OK] C compiler found"
	@test -d $(INC_DIR) || (echo "ERROR: include/ missing"; exit 1)
	@test -d $(SRC_DIR) || (echo "ERROR: src/ missing"; exit 1)
	@echo "  [OK] include/ and src/ present"
	@$(MAKE) lib USE_MONGOC=0 || (echo "ERROR: Library build failed. Run: make lib USE_MONGOC=0"; exit 1)
	@echo "  [OK] lib builds"
	@echo "=== Validation passed ==="

# Expose object files for consumers that want to link .o directly (e.g. inject into build)
lib-o: $(LIB_OBJECTS)
	@echo "Library object files: $(LIB_OBJECTS)"

clean:
	rm -rf $(BUILD) $(LIB_DIR) $(DIST_DIR)

# ------------------------------------------------------------------------------
# Smart topic (mini AI) — default TINY model via Ollama
# ------------------------------------------------------------------------------
# Default model name for MINI_AI_TYPE_TINY (see include/smart_topic.h, .cursor/smart_topic_ai_switch.md).
OLLAMA_TINY_MODEL ?= tinyllama
OLLAMA_HOST      ?= 127.0.0.1
OLLAMA_PORT      ?= 11434

# DISCUSSION: System vs optional Ollama / TINY model
# - Smart topic (initial_smart_topic / get_smart_topic) can use a small model (e.g. tinyllama) for
#   topic routing and cache. The library talks to Ollama at 127.0.0.1:11434; it does NOT bundle or
#   install Ollama or any model.
# - Option A (system): Require Ollama + default TINY model to be installed and running at system
#   level (e.g. brew install ollama; ollama serve; ollama pull tinyllama). CI and deploy scripts
#   run `make ollama-tiny` or equivalent. No install from this repo.
# - Option B (optional): Smart topic is optional. If Ollama is not available, get_smart_topic
#   returns "ollama_unavailable" and the app uses cache-only or skips mini AI. No Makefile install.
# - Option C (install-if-missing): Makefile targets below help the developer: `make ollama-tiny`
#   pulls the TINY model when Ollama is on PATH; `make ollama-serve` starts the server if not
#   listening. This is best-effort for local dev; production should use system/container setup.
# This repo prefers Option C for dev convenience, with Option A/B for production (no automatic
# system install; operator installs Ollama and model if smart topic is required).

.PHONY: ollama-tiny ollama-serve ollama-check smart-topic-deps

# Ensure default TINY model is pulled. Requires Ollama on PATH. Idempotent if already present.
ollama-tiny: ollama-check
	@echo "Pulling default TINY model: $(OLLAMA_TINY_MODEL)"
	@ollama pull $(OLLAMA_TINY_MODEL) || (echo "WARN: ollama pull $(OLLAMA_TINY_MODEL) failed (run 'ollama serve' first?)"; exit 1)

# Start Ollama server in background if not already listening. Best-effort for local dev.
ollama-serve:
	@curl -s -o /dev/null -w "%{http_code}" "http://$(OLLAMA_HOST):$(OLLAMA_PORT)/api/tags" 2>/dev/null | grep -q 200 && \
		echo "Ollama already running at $(OLLAMA_HOST):$(OLLAMA_PORT)" || \
		(command -v ollama >/dev/null 2>&1 && (ollama serve &) && echo "Started ollama serve (background)" || echo "WARN: ollama not on PATH; install e.g. brew install ollama")

# Check Ollama is on PATH. Fails if not (so ollama-tiny fails with a clear message).
ollama-check:
	@command -v ollama >/dev/null 2>&1 || (echo "ERROR: ollama not on PATH. Install e.g. brew install ollama (see DISCUSSION in Makefile)"; exit 1)

# Install and run if not available: pull TINY model and optionally start server. Use for local dev.
smart-topic-deps: ollama-check
	@curl -s -o /dev/null -w "%{http_code}" "http://$(OLLAMA_HOST):$(OLLAMA_PORT)/api/tags" 2>/dev/null | grep -q 200 || \
		(echo "Ollama not responding at $(OLLAMA_HOST):$(OLLAMA_PORT); starting ollama serve..."; command -v ollama >/dev/null 2>&1 && (ollama serve &) && sleep 2 || true)
	@$(MAKE) ollama-tiny

# ------------------------------------------------------------------------------
# Language detection (lang.c) — metadata.lang / metadata.score per .cursor/language.md
# ------------------------------------------------------------------------------
# lang_detect() in lang.c uses Unicode script blocks (CJK, Thai, Arabic, Vietnamese).
# Mixed when multiple scripts >15% each. No CLD3, no Python.
.PHONY: validate-lang
validate-lang:
	@echo "=== Validate lang (language.md) ==="
	@mkdir -p $(BUILD)
	@$(CC) $(CFLAGS) -I$(INC_DIR) -o $(BUILD)/validate_lang tests/validate_lang.c $(SRC_DIR)/lang.c
	@$(BUILD)/validate_lang || (echo "ERROR: validate-lang failed"; exit 1)

# ------------------------------------------------------------------------------
# Unit tests: api source (M/R/G/O), get_history_message source_out, Redis stub
# ------------------------------------------------------------------------------
.PHONY: test-api-source-redis
test-api-source-redis: lib-static
	@echo "=== api source + Redis stub unit tests ==="
	@mkdir -p $(BUILD)
	@$(CC) $(CFLAGS) -I$(INC_DIR) -o $(BUILD)/api_source_and_redis_test tests/api_source_and_redis_test.c $(LIB_DIR)/libm4engine.a $(LDFLAGS)
	@$(BUILD)/api_source_and_redis_test || (echo "ERROR: test-api-source-redis failed"; exit 1)

# model_switch_resolve: lanes table, merge flag, session override (no Ollama)
.PHONY: test-model-switch-flow
test-model-switch-flow: lib-static
	@echo "=== model_switch flow (unit) ==="
	@mkdir -p $(BUILD)
	@$(CC) $(CFLAGS) -I$(INC_DIR) -o $(BUILD)/model_switch_flow_test tests/model_switch_flow_test.c $(LIB_DIR)/libm4engine.a $(LDFLAGS)
	@$(BUILD)/model_switch_flow_test || (echo "ERROR: test-model-switch-flow failed"; exit 1)

.PHONY: test-vector-generate
test-vector-generate:
	@echo "=== vector_generate (unit) ==="
	@mkdir -p $(BUILD)
	@$(CC) $(CFLAGS) -I$(INC_DIR) -o $(BUILD)/vector_generate_test tests/vector_generate_test.c $(SRC_DIR)/vector_generate.c -lm
	@$(BUILD)/vector_generate_test || (echo "ERROR: test-vector-generate failed"; exit 1)
