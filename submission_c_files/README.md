# SyncLab — Submission C Programs (Readers-Writers Algorithms)

This directory contains standalone, self-contained, fully-commented C source files implementing the three classic Readers-Writers synchronization algorithms using POSIX threads (`pthread`) and POSIX semaphores (`sem_t`).

---

## 1. Directory Contents & Order

| File | Algorithm | Mechanism | Starvation Consequence |
|---|---|---|---|
| `01_reader_preference.c` | First Readers-Writers Problem (Courtois et al. 1971) | First reader locks `resource`; subsequent readers read concurrently under `mutex`. | High throughput; continuous reader stream causes **Writer Starvation**. |
| `02_writer_preference.c` | Second Readers-Writers Problem (Courtois et al. 1971) | Arriving writer locks `read_try` gate to block incoming readers. | Writers have priority; continuous writer stream causes **Reader Starvation**. |
| `03_fair_fifo.c` | Third Readers-Writers Problem (Starvation-Free Turnstile) | Arriving threads queue at `order` FIFO turnstile in strict order of arrival. | Strictly fair request ordering; **Zero Starvation** for both readers and writers. |
| `Makefile` | Automated build system | Builds all three standalone binaries. | `make` / `make run-all` |

---

## 2. Compilation and Execution

### Using Make (Linux / WSL / MinGW):
```bash
make
./01_reader_preference
./02_writer_preference
./03_fair_fifo
```
Or run all three back-to-back:
```bash
make run-all
```

### Direct GCC Compilation:
```bash
# 1. Reader Preference
gcc 01_reader_preference.c -pthread -o 01_reader_pref && ./01_reader_pref

# 2. Writer Preference
gcc 02_writer_preference.c -pthread -o 02_writer_pref && ./02_writer_pref

# 3. Fair FIFO Turnstile
gcc 03_fair_fifo.c -pthread -o 03_fair_fifo && ./03_fair_fifo
```

---

## 3. Code Verification
Each program demonstrates:
1. Mutual exclusion between readers and writers.
2. Multiple concurrent readers.
3. Exclusive single-writer access with monotonic version tracking.
4. Clean thread join and semaphore teardown with zero resource leakage.
