# M4-Hardcore AI Engine — Makefile
# Apple Silicon M4 / RTX 4070 heterogeneous build

CC       = clang
CFLAGS   = -std=c17 -Wall -Wextra -O3
# Apple Silicon: use -arch arm64 on macOS so local builds are arm64; CI (Linux) omits it
ifeq ($(shell uname -s),Darwin)
  CFLAGS += -arch arm64
endif
LDFLAGS  = -lm -lncurses -lpthread -lcurl

# Optional: MongoDB C Driver (libmongoc). Set USE_MONGOC=1 to enable chat persistence to MongoDB.
# Homebrew: brew install mongo-c-driver (uses mongoc-2.x, bson-2.x under opt/mongo-c-driver)
MONGODB_ROOT ?= $(shell brew --prefix mongo-c-driver 2>/dev/null || echo "/opt/homebrew")
REDIS_ROOT   ?= /opt/homebrew
USE_MONGOC   ?= 0
ifeq ($(USE_MONGOC),1)
# Prefer Homebrew layout (mongoc-2.2.3, bson-2.2.3); fallback to libmongoc-2.0 / libbson-2.0
MONGOC_INC   := $(wildcard $(MONGODB_ROOT)/include/mongoc-*) $(wildcard $(MONGODB_ROOT)/include/bson-*)
ifeq ($(MONGOC_INC),)
MONGOC_INC   := $(MONGODB_ROOT)/include/libmongoc-2.0 $(MONGODB_ROOT)/include/libbson-2.0
MONGOC_LIBS  := -lmongoc-2.0 -lbson-2.0
else
MONGOC_LIBS  := -lmongoc2 -lbson2
endif
CFLAGS  += -DUSE_MONGOC $(addprefix -I,$(MONGOC_INC))
LDFLAGS += -L$(MONGODB_ROOT)/lib $(MONGOC_LIBS)
endif
# LDFLAGS += -L$(REDIS_ROOT)/lib -lhiredis

# C library subproject; consumers: c_ai (C test app), python_ai (Python)
C_LIB     = c-lib
SRC_DIR   = $(C_LIB)/src
INC_DIR   = $(C_LIB)/include
BUILD      = build
BIN        = bin

SOURCES  = $(SRC_DIR)/main.c $(SRC_DIR)/engine.c $(SRC_DIR)/tenant.c \
           $(SRC_DIR)/dispatcher.c $(SRC_DIR)/storage.c $(SRC_DIR)/validate.c \
           $(SRC_DIR)/ollama.c $(SRC_DIR)/stat.c $(SRC_DIR)/api.c $(SRC_DIR)/terminal_ui.c $(SRC_DIR)/debug_monitor.c
OBJECTS  = $(patsubst $(SRC_DIR)/%.c,$(BUILD)/%.o,$(SOURCES))
TARGET   = $(BIN)/ai_bot
# Stamp so changing USE_MONGOC triggers rebuild
CONFIG_STAMP = $(BUILD)/.config_use_mongoc_$(USE_MONGOC)

# Shared library for use from Python, Java, Node, etc. (see docs/TUTORIAL_BINDINGS.md)
LIB_SOURCES = $(SRC_DIR)/engine.c $(SRC_DIR)/tenant.c $(SRC_DIR)/dispatcher.c \
              $(SRC_DIR)/storage.c $(SRC_DIR)/validate.c $(SRC_DIR)/ollama.c $(SRC_DIR)/stat.c $(SRC_DIR)/api.c
LIB_OBJECTS = $(patsubst $(SRC_DIR)/%.c,$(BUILD)/lib_%.o,$(LIB_SOURCES))
UNAME_S    := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  LIB_NAME = libm4engine.dylib
  LIB_LDFLAGS = -dynamiclib -install_name @rpath/$(LIB_NAME)
else
  LIB_NAME = libm4engine.so
  LIB_LDFLAGS = -shared
endif
LIB_DIR   = lib
# Version and platform for dist/ and private registry (e.g. m4engine-1.0.0-darwin-arm64)
VERSION   := $(shell grep -E 'ENGINE_VERSION' $(INC_DIR)/engine.h 2>/dev/null | sed 's/.*"\([^"]*\)".*/\1/' || echo "1.0.0")
UNAME_S   := $(shell uname -s)
UNAME_M   := $(shell uname -m)
OS        := $(shell echo "$(UNAME_S)" | tr '[:upper:]' '[:lower:]')
ARCH      := $(shell echo "$(UNAME_M)" | sed 's/x86_64/amd64/;s/aarch64/arm64/')
DIST_NAME = m4engine-$(VERSION)-$(OS)-$(ARCH)
DIST_DIR  = dist

.PHONY: all clean validate run watch lib lib-static package publish c_ai

all: $(TARGET)

$(TARGET): $(OBJECTS)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(CONFIG_STAMP):
	@mkdir -p $(BUILD)
	@rm -f $(BUILD)/.config_use_mongoc_0 $(BUILD)/.config_use_mongoc_1
	@touch $@

$(BUILD)/%.o: $(SRC_DIR)/%.c $(CONFIG_STAMP)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(INC_DIR) -c -o $@ $<

# Object files for shared lib (with -fPIC)
$(BUILD)/lib_%.o: $(SRC_DIR)/%.c $(CONFIG_STAMP)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fPIC -I$(INC_DIR) -c -o $@ $<

$(LIB_DIR)/$(LIB_NAME): $(LIB_OBJECTS)
	@mkdir -p $(LIB_DIR)
	$(CC) $(LIB_LDFLAGS) -o $@ $(LIB_OBJECTS) $(LDFLAGS)

# Static library .a for linking into other C/C++ apps or shipping to a private registry
$(LIB_DIR)/libm4engine.a: $(LIB_OBJECTS)
	@mkdir -p $(LIB_DIR)
	$(AR) rcs $@ $(LIB_OBJECTS)
	$(RANLIB) $@ 2>/dev/null || true

AR      ?= ar
RANLIB  ?= ranlib

lib: $(LIB_DIR)/$(LIB_NAME)
lib-static: $(LIB_DIR)/libm4engine.a
lib-all: $(LIB_DIR)/$(LIB_NAME) $(LIB_DIR)/libm4engine.a

# Package for private registry: include/ + lib/*.a + lib/*.so|.dylib + build/*.o + VERSION
# Set INCLUDE_OBJ=1 to add build/lib_*.o to the tarball (e.g. make package INCLUDE_OBJ=1)
package: lib lib-static
	@mkdir -p $(DIST_DIR)/$(DIST_NAME)/include $(DIST_DIR)/$(DIST_NAME)/lib
	cp $(INC_DIR)/*.h $(DIST_DIR)/$(DIST_NAME)/include/
	cp $(LIB_DIR)/libm4engine.a $(LIB_DIR)/$(LIB_NAME) $(DIST_DIR)/$(DIST_NAME)/lib/
	@echo "$(VERSION)" > $(DIST_DIR)/$(DIST_NAME)/VERSION
	@echo "OS=$(OS) ARCH=$(ARCH) USE_MONGOC=$(USE_MONGOC)" > $(DIST_DIR)/$(DIST_NAME)/BUILD_INFO
ifeq ($(INCLUDE_OBJ),1)
	@mkdir -p $(DIST_DIR)/$(DIST_NAME)/build
	cp $(LIB_OBJECTS) $(DIST_DIR)/$(DIST_NAME)/build/
endif
	(cd $(DIST_DIR) && tar czf $(DIST_NAME).tar.gz $(DIST_NAME))
	@echo "Created $(DIST_DIR)/$(DIST_NAME).tar.gz"

validate:
	@echo "=== Validating environment (M4-Hardcore AI Engine) ==="
	@command -v $(CC) >/dev/null 2>&1 || (echo "ERROR: clang not found"; exit 1)
	@echo "  [OK] clang: $$(clang --version | head -1)"
	@test -d $(C_LIB) || (echo "ERROR: c-lib/ missing"; exit 1)
	@test -d $(INC_DIR) || (echo "ERROR: c-lib/include/ missing"; exit 1)
	@test -d $(SRC_DIR) || (echo "ERROR: c-lib/src/ missing"; exit 1)
	@echo "  [OK] c-lib (include + src) present"
	@echo "=== Validation passed ==="

# C test app: build lib then c_ai (reference consumer of c-lib)
c_ai: lib
	$(MAKE) -C c_ai

clean:
	rm -rf $(BUILD) $(BIN) $(LIB_DIR) $(DIST_DIR)

run: $(TARGET)
	./$(TARGET) --mode hybrid

# Watch src/, include/, and Makefile; rebuild and run on change (requires: brew install entr).
# Pass build options to inner make, e.g.: make watch USE_MONGOC=1
WATCH_MAKE = $(MAKE) run USE_MONGOC=$(USE_MONGOC) MONGODB_ROOT="$(MONGODB_ROOT)"
watch:
	@command -v entr >/dev/null 2>&1 || (echo "Install entr: brew install entr"; exit 1)
	(find $(SRC_DIR) $(INC_DIR) -type f \( -name '*.c' -o -name '*.h' \) ; echo Makefile) | entr -c $(WATCH_MAKE)
