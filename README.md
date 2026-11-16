# SyncLab — Reader–Writer Synchronization Simulator

An interactive operating-systems laboratory for understanding concurrent readers, exclusive writers, semaphore behavior, synchronization fairness, and starvation.

Built using a **C backend** (POSIX **pthreads** + **semaphores**, hand-written HTTP/WebSocket server), **React 19**, **Vite**, **TypeScript**, and **Tailwind CSS**, designed under strict technical, editorial **Hallmark** light-theme design principles.

---

## 1. System Architecture

```
synclab/
├── submission_c_files/               # Standalone C demonstration programs (for lab submission & viva)
│   ├── 01_reader_preference.c        # Standalone Algorithm 1 (Reader Preference)
│   ├── 02_writer_preference.c        # Standalone Algorithm 2 (Writer Preference)
│   ├── 03_fair_fifo.c                # Standalone Algorithm 3 (Fair FIFO Turnstile)
│   ├── Makefile                      # make | make run-all | make clean
│   └── README.md                     # Direct gcc commands & algorithm overview
├── backend/                          # Full Simulator C Backend (C11 / POSIX)
│   ├── Makefile                      # make | make run | make test | make clean
│   ├── src/
│   │   ├── server.c                  # HTTP + WebSocket server, REST routes, static hosting
│   │   ├── sync.h / sync.c           # Protected resource, 3 semaphore algorithms, simulation engine
│   │   └── util.h / util.c           # String buffer, JSON reader/writer, monotonic timers
│   └── tests/
│       ├── test_sync.c               # 15 C unit/concurrency tests (invariants, fairness, stress...)
│       ├── api_smoke.sh              # Starts server and runs end-to-end test suite
│       └── e2e_*.py                  # REST + WebSocket integration tests
├── frontend/                         # React UI (Vite + TypeScript + Tailwind CSS)
├── start_all.bat                     # One-click Windows full-stack launcher
├── start_backend.bat                 # Launches C backend in WSL
└── start_frontend.bat                # Launches Vite frontend dev server
```

---

## 2. Formal Safety Invariants

1. **Reader-Writer Exclusion:** No reader may hold a read lease while a writer holds an exclusive write lease (`InvariantViolationError` raised on breach).
2. **Writer Mutual Exclusion:** At most one writer may access the resource at any given moment.
3. **Reader Concurrency:** Multiple compatible reader threads may access the shared memory segment concurrently.
4. **Monotonic Versioning:** Every committed write atomically increments the resource version counter (`v1, v2, v3...`).
5. **Clean Cancellation:** Stopping or resetting signals all active worker threads via POSIX-compliant thread events without thread or semaphore leakage.

---

## 3. Implemented Synchronization Algorithms

### Algorithm 1: Reader Preference (Courtois et al. 1971)
* **Mechanics:** The first arriving reader locks the shared resource (`resource_lock.acquire()`). Subsequent readers bypass the lock and read concurrently. The last exiting reader unlocks the resource (`resource_lock.release()`).
* **Consequence:** Writers wait as long as `read_count > 0`. A continuous stream of readers can cause **indefinite writer starvation**.

### Algorithm 2: Writer Preference (Courtois et al. 1971)
* **Mechanics:** When any writer queues, it acquires the `read_try` gate. Newly arriving readers are blocked at `read_try` and cannot enter. Active readers finish, after which the waiting writer claims the resource lock.
* **Consequence:** Writers receive priority service. A continuous stream of writers can cause **reader starvation**.

### Algorithm 3: Fair FIFO Admission (Starvation-Free Turnstile)
* **Mechanics:** All arriving threads pass through a single-ticket FIFO turnstile (`order_queue`). Consecutive readers still execute concurrently, but newly arriving readers cannot bypass an earlier queued writer.
* **Consequence:** Strictly fair request ordering. **Zero starvation** for both readers and writers.

---

## 4. How to Run

The backend needs **Linux** (or **WSL** on Windows). macOS is not supported because it lacks unnamed `sem_t` semaphores. Requirements: `gcc` (or `cc`) and `make`.

```bash
cd synclab/backend
make              # builds build/synclab_server
make run          # http://127.0.0.1:8000  (serves frontend/dist + the API + /ws/events)
./build/synclab_server --host 127.0.0.1 --port 8000 --dist ../frontend/dist
```
Or run `./start_backend.sh` (Linux/WSL) / double-click `start_backend.bat` (Windows, uses WSL).

For frontend development: `cd frontend && npm run dev` (Vite proxies `/api` and `/ws` to port 8000). The UI is unchanged and talks to the same endpoints as before.

---

## 5. Tests

```bash
cd synclab/backend
make test
```
Runs `test_sync` (15 C tests: resource invariants, concurrent readers, reader-/writer-preference and FIFO behaviour, cancellation, pause/resume, reset, timeout, 100x50 stress for all algorithms) and then the end-to-end REST/WebSocket scripts against a live server. The C code was additionally run clean under ThreadSanitizer and AddressSanitizer/UBSan.

---

## 6. Advanced Synchronization & Systems Engineering Notes

### 6.1 Cooperative Mid-Flight Pause/Resume & Lease Safety
* **The Classical Trap:** Arbitrarily suspending an in-flight OS thread holding an acquired mutex/semaphore via raw kernel signals (`SIGSTOP` or `SuspendThread`) induces priority inversion and immediate deadlock: other threads waiting on that lock cannot make progress, and if the supervisor depends on downstream state, the process hangs.
* **SyncLab's Solution:** SyncLab implements **cooperative slice barriers**. Worker threads check a `pause_event` condition every 20ms:
  1. If paused while in the critical section, the thread **retains its acquired lease** (`resource_lock` or reader count), freezing its progress bar without releasing locks prematurely.
  2. If paused while waiting in line, the thread stays queued without losing its turnstile position.
  3. When resumed, `pause_event.set()` signals workers to immediately resume execution.
  4. **Time Dilation:** Cumulative paused durations (`total_paused_sec`) are subtracted from monotonic execution timestamps so throughput, duration, and waiting-time metrics remain mathematically unskewed.

### 6.2 Native Threads & POSIX Semaphores
* Every reader/writer is a real `pthread`; `sem_t` semaphores (`sem_timedwait` in 100 ms slices) implement the algorithms, so stop/pause are honoured while a thread is blocked, with no leaked semaphores or leases.
* The first reader holds the counter mutex while it waits for the writer lock (the textbook solution), which is what guarantees readers can never overlap a writer.
* Resource values saturate at +/-(2^53-1) so JSON numbers stay exact in the browser.

### 6.3 High-Density Scalability (100 Readers & 50 Writers)
* The workload engine supports up to **100 reader threads** and **50 writer threads** concurrently.
* The frontend visualization provides an information-dense, scrollable grid with segmented state filters (`ALL`, `ACTIVE`, `WAITING`, `COMPLETED`), allowing operators to monitor large thread pools without browser slowdown.

---

## 7. Visual Design System (The Concurrency Workbench)

* **Design Standard:** Audited and verified against Hallmark rules with `sloplint.mjs` returning **clean (0 FAIL, 0 REFLEX, 0 WARN)**.
* **Concurrency Workbench Tokens:**
  * Canvas: Warm neutral `#F7F7F4` (`--color-paper`)
  * Primary Surfaces: Clean neutral `#FCFCFA` (`--color-surface`)
  * Elevated Neutral: `#F0F0EC` (`--color-surface-elevated`)
  * Borders / Hairlines: Fine 1px neutral `#E5E5DE` (`--color-rule`)
  * Primary Ink: Deep charcoal `#242622` (`--color-ink`)
  * Secondary Text: Restrained warm gray `#74766F` (`--color-ink-muted`)
  * Primary Accent: Muted industrial teal `#426B63` (`--color-accent`)
  * Reader Activity: Restrained green `#56816B` (`--state-reader`)
  * Writer Activity: Terracotta / copper `#B66C4F` (`--state-writer`)
  * Waiting Processes: Warm amber `#B18A45` (`--state-waiting`)
  * Critical / Deadlock: Controlled red `#B94C43` (`--state-critical`)
* **Typography:** `Geist` / `Inter` (technical UI typography) and `Geist Mono` / `JetBrains Mono` (PIDs, wait times, addresses, telemetry & trace steps).
* **Strict Anti-Slop Discipline:**
  * Zero blue-purple gradients, zero neon glows, zero cards inside cards.
  * Zero fake metrics: every displayed number and event is verified by backend telemetry.
  * Hairline 1px borders, zero drop shadows (`--shadow-card: none`), razor corner radiuses `rounded-[2px]`.
  * Physical systems instrument architecture: Concurrent Reader Bus, Protected Register 0x01, and Exclusive Writer Clamp.

