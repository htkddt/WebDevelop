# c_ai — C test app that links the c-lib. Build from repo root: make lib && make -C c_ai
ROOT   = ..
C_LIB  = $(ROOT)/c-lib
LIB    = $(ROOT)/lib
CC     = clang
CFLAGS = -std=c17 -Wall -O2 -I$(C_LIB)/include
LDFLAGS = -L$(LIB) -lm4engine -lm -lpthread -lcurl
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  LDFLAGS += -Wl,-rpath,@loader_path/../lib
else
  LDFLAGS += -Wl,-rpath,'$$ORIGIN/../lib'
endif

TARGET = c_ai_bot
SRC    = src/main.c

.PHONY: all run clean

all: $(TARGET)

$(TARGET): $(SRC)
	@test -f $(LIB)/libm4engine.a || test -f $(LIB)/libm4engine.so || test -f $(LIB)/libm4engine.dylib || (echo "Build the lib first: make lib"; exit 1)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

run: $(TARGET)
	./$(TARGET) "Hello, respond in one short sentence."

clean:
	rm -f $(TARGET)
