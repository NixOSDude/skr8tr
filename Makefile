# Skr8tr — Sovereign Orchestrator
# C23 daemon build system

CC      = gcc
# gnu23: C23 + POSIX extensions (nanosleep, putenv, kill, etc.)
CFLAGS  = -O3 -Wall -Wextra -std=gnu23 -I./src/core
LDFLAGS = -lpthread -loqs

OQS_INC ?= $(shell pkg-config --variable=includedir liboqs 2>/dev/null || \
             find /nix/store -name "oqs.h" 2>/dev/null | head -1 | xargs dirname | xargs dirname)
OQS_LIB ?= $(shell pkg-config --libs-only-L liboqs 2>/dev/null | sed 's/-L//' || \
             find /nix/store -name "liboqs.so" 2>/dev/null | head -1 | xargs dirname 2>/dev/null)

ifneq ($(OQS_INC),)
  CFLAGS  += -I$(OQS_INC)/include
  LDFLAGS += -L$(OQS_LIB)
endif

BIN = bin
SRC = src

.PHONY: all clean

all: $(BIN) \
     $(BIN)/skr8tr_node \
     $(BIN)/skr8tr_sched \
     $(BIN)/skr8tr_reg \
     $(BIN)/skr8tr_serve \
     $(BIN)/skrtrkey

$(BIN):
	mkdir -p $(BIN)

$(BIN)/skr8tr_node: $(SRC)/daemon/skr8tr_node.c $(SRC)/core/fabric.c \
                    $(SRC)/parser/skrmaker.c
	$(CC) $(CFLAGS) -I./src/parser $^ -o $@ $(LDFLAGS)

$(BIN)/skr8tr_sched: $(SRC)/daemon/skr8tr_sched.c $(SRC)/core/fabric.c \
                    $(SRC)/core/skrauth.c $(SRC)/parser/skrmaker.c
	$(CC) $(CFLAGS) -I./src/parser $^ -o $@ $(LDFLAGS)

$(BIN)/skr8tr_reg: $(SRC)/daemon/skr8tr_reg.c $(SRC)/core/fabric.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN)/skr8tr_serve: $(SRC)/server/skr8tr_serve.c
	$(CC) $(CFLAGS) $^ -o $@ -lpthread

$(BIN)/skrtrkey: $(SRC)/tools/skrtrkey.c $(SRC)/core/skrauth.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -rf $(BIN)
