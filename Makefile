# Skr8tr — Sovereign Orchestrator
# C23 daemon build system

CC      = gcc
# gnu23: C23 + POSIX extensions (nanosleep, putenv, kill, etc.)
# Set ENTERPRISE=1 to build with private enterprise features (audit, syslog, RBAC)
# Private Gitea builds use: make ENTERPRISE=1
# Public open-source builds: make  (no flag)
ENTERPRISE_FLAGS =
ifdef ENTERPRISE
  ENTERPRISE_FLAGS = -DENTERPRISE -I./src/enterprise
endif
CFLAGS  = -O3 -Wall -Wextra -std=gnu23 -I./src/core $(ENTERPRISE_FLAGS)
LDFLAGS = -lpthread -loqs

OQS_INC ?= $(shell pkg-config --variable=includedir liboqs 2>/dev/null)
ifeq ($(OQS_INC),)
OQS_INC := $(shell find /nix/store -name "oqs.h" 2>/dev/null | head -1 | xargs dirname 2>/dev/null | xargs dirname 2>/dev/null)
endif

OQS_LIB ?= $(shell pkg-config --variable=libdir liboqs 2>/dev/null)
ifeq ($(OQS_LIB),)
OQS_LIB := $(shell find /nix/store -name "liboqs.so" 2>/dev/null | head -1 | xargs dirname 2>/dev/null)
endif

ifneq ($(OQS_INC),)
  CFLAGS  += -I$(OQS_INC)
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
     $(BIN)/skr8tr_ingress \
     $(BIN)/skrtrkey

$(BIN):
	mkdir -p $(BIN)

$(BIN)/skr8tr_node: $(SRC)/daemon/skr8tr_node.c $(SRC)/core/fabric.c \
                    $(SRC)/parser/skrmaker.c
	$(CC) $(CFLAGS) -I./src/parser $^ -o $@ $(LDFLAGS)

SCHED_SRCS = $(SRC)/daemon/skr8tr_sched.c $(SRC)/core/fabric.c \
             $(SRC)/core/skrauth.c $(SRC)/parser/skrmaker.c
ifdef ENTERPRISE
  SCHED_SRCS += $(SRC)/enterprise/skr8tr_audit.c \
                $(SRC)/enterprise/skr8tr_syslog.c
endif

$(BIN)/skr8tr_sched: $(SCHED_SRCS)
	$(CC) $(CFLAGS) -I./src/parser $^ -o $@ $(LDFLAGS) -lssl -lcrypto

$(BIN)/skr8tr_reg: $(SRC)/daemon/skr8tr_reg.c $(SRC)/core/fabric.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN)/skr8tr_serve: $(SRC)/server/skr8tr_serve.c
	$(CC) $(CFLAGS) $^ -o $@ -lpthread

$(BIN)/skr8tr_ingress: $(SRC)/daemon/skr8tr_ingress.c $(SRC)/core/fabric.c
	$(CC) $(CFLAGS) $^ -o $@ -lpthread -lssl -lcrypto

$(BIN)/skrtrkey: $(SRC)/tools/skrtrkey.c $(SRC)/core/skrauth.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -rf $(BIN)
