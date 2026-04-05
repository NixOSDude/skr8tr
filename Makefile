# Skr8tr — Sovereign Orchestrator
# C23 daemon build system

CC      = gcc
CFLAGS  = -O3 -Wall -Wextra -std=c23 -I./src/core
LDFLAGS = -lpthread -loqs

OQS_INC ?= $(shell pkg-config --cflags liboqs 2>/dev/null || \
             find /nix/store -name "oqs.h" 2>/dev/null | head -1 | xargs dirname | xargs dirname)
OQS_LIB ?= $(shell pkg-config --libs-only-L liboqs 2>/dev/null || \
             find /nix/store -name "liboqs.so" -o -name "liboqs.a" 2>/dev/null | \
             head -1 | xargs dirname 2>/dev/null)

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
     $(BIN)/skr8tr_serve

$(BIN):
	mkdir -p $(BIN)

$(BIN)/skr8tr_node: $(SRC)/daemon/skr8tr_node.c $(SRC)/core/fabric.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN)/skr8tr_sched: $(SRC)/daemon/skr8tr_sched.c $(SRC)/core/fabric.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN)/skr8tr_reg: $(SRC)/daemon/skr8tr_reg.c $(SRC)/core/fabric.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN)/skr8tr_serve: $(SRC)/server/skr8tr_serve.c
	$(CC) $(CFLAGS) $^ -o $@ -lpthread

clean:
	rm -rf $(BIN)
