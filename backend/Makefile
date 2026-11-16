# SyncLab backend (C) - POSIX threads + POSIX semaphores
CC      ?= gcc
CFLAGS  ?= -std=gnu11 -O2 -Wall -Wextra -pthread
LDFLAGS ?= -pthread -lm

BUILD   := build
SRC     := src/server.c src/sync.c src/util.c
LIBSRC  := src/sync.c src/util.c

.PHONY: all run test clean

all: $(BUILD)/synclab_server

$(BUILD)/synclab_server: $(SRC) src/sync.h src/util.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

$(BUILD)/test_sync: tests/test_sync.c $(LIBSRC) src/sync.h src/util.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_sync.c $(LIBSRC) $(LDFLAGS)

run: all
	./$(BUILD)/synclab_server

test: $(BUILD)/test_sync all
	./$(BUILD)/test_sync
	sh tests/api_smoke.sh

clean:
	rm -rf $(BUILD)
