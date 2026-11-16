# Makefile for SyncLab Submission C Programs
CC      ?= gcc
CFLAGS  ?= -std=gnu11 -O2 -Wall -Wextra -pthread
LDFLAGS ?= -pthread

TARGETS := 01_reader_preference 02_writer_preference 03_fair_fifo

.PHONY: all clean run-all

all: $(TARGETS)

01_reader_preference: 01_reader_preference.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

02_writer_preference: 02_writer_preference.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

03_fair_fifo: 03_fair_fifo.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

run-all: all
	@echo "=== Running 01_reader_preference ==="
	./01_reader_preference
	@echo "\n=== Running 02_writer_preference ==="
	./02_writer_preference
	@echo "\n=== Running 03_fair_fifo ==="
	./03_fair_fifo

clean:
	rm -f $(TARGETS) *.exe
