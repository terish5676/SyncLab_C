#define _GNU_SOURCE
#include "sync.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ======================= names ======================= */

const char *algo_name(Algo a) {
    static const char *n[] = { "reader_preference", "writer_preference", "fair_fifo" };
    return n[a];
}
const char *wop_name(WOp o) {
    static const char *n[] = { "ADD", "SUBTRACT", "MULTIPLY", "SET" };
    return n[o];
}
const char *ptype_name(PType t) { return t == PT_READER ? "READER" : "WRITER"; }
const char *pstate_name(PState s) {
    static const char *n[] = { "CREATED", "WAITING", "READING", "WRITING", "COMPLETED", "CANCELLED", "ERROR" };
    return n[s];
}
const char *status_name(SimStatus s) {
    static const char *n[] = { "IDLE", "RUNNING", "PAUSED", "COMPLETED", "CANCELLED", "ERROR" };
    return n[s];
}
int algo_parse(const char *s, Algo *out) {
    for (int i = 0; i < 3; i++)
        if (strcmp(s, algo_name((Algo)i)) == 0) { *out = (Algo)i; return 1; }
    return 0;
}
int wop_parse(const char *s, WOp *out) {
    for (int i = 0; i < 4; i++)
        if (strcmp(s, wop_name((WOp)i)) == 0) { *out = (WOp)i; return 1; }
    return 0;
}

/* ======================= Flag ======================= */

void flag_init(Flag *f) {
    pthread_condattr_t a;
    pthread_mutex_init(&f->m, NULL);
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&f->c, &a);
    pthread_condattr_destroy(&a);
    f->flag = 0;
}
void flag_destroy(Flag *f) {
    pthread_cond_destroy(&f->c);
    pthread_mutex_destroy(&f->m);
}
void flag_set(Flag *f) {
    pthread_mutex_lock(&f->m);
    f->flag = 1;
    pthread_cond_broadcast(&f->c);
    pthread_mutex_unlock(&f->m);
}
void flag_clear(Flag *f) {
    pthread_mutex_lock(&f->m);
    f->flag = 0;
    pthread_mutex_unlock(&f->m);
}
int flag_is_set(Flag *f) {
    pthread_mutex_lock(&f->m);
    int v = f->flag;
    pthread_mutex_unlock(&f->m);
    return v;
}
int flag_wait(Flag *f, double timeout_sec) {
    pthread_mutex_lock(&f->m);
    if (!f->flag && timeout_sec > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        long ns = (long)((timeout_sec - floor(timeout_sec)) * 1e9);
        ts.tv_sec += (time_t)floor(timeout_sec);
        ts.tv_nsec += ns;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        while (!f->flag) {
            if (pthread_cond_timedwait(&f->c, &f->m, &ts) == ETIMEDOUT) break;
        }
    }
    int v = f->flag;
    pthread_mutex_unlock(&f->m);
    return v;
}

/* =========================================================================
 *              PROTECTED SHARED RESOURCE & SAFETY INVARIANTS
 * =========================================================================
 * The Resource struct models the shared register (Register 0x01).
 * Every entry and exit formally asserts the fundamental safety invariants:
 *   - Invariant 1 (Reader-Writer Exclusion): No reader may hold a lease while a writer is active.
 *   - Invariant 2 (Writer Mutual Exclusion): At most one writer may access the resource.
 *   - Invariant 3 (Monotonic Versioning): Every committed write atomically increments version.
 *   - Invariant 4 (Lease Integrity): No thread may read/write without holding a valid lease.
 */

void res_init(Resource *r, int64_t initial) {
    memset(r, 0, sizeof *r);
    pthread_mutex_init(&r->m, NULL);
    r->value = initial;
    r->active_writer = -1;
    r->last_change = mono_now();
}
void res_destroy(Resource *r) { pthread_mutex_destroy(&r->m); }

#define VIOLATION(...) do { if (err) snprintf(err, en, __VA_ARGS__); rc = -1; goto out; } while (0)

/*
 * res_enter_read: Registers an admitted reader.
 * Invariant 1 Check: Asserts active_writer < 0 (no writer in critical section).
 */
int res_enter_read(Resource *r, int idx, const char *pid, char *err, size_t en) {
    int rc = 0;
    pthread_mutex_lock(&r->m);
    if (r->active_writer >= 0)
        VIOLATION("Invariant 1 Violated: Reader %s entered while writer %s is active!", pid, r->active_writer_id);
    if (!r->is_occupied) { r->is_occupied = 1; r->last_change = mono_now(); }
    if (!r->active[idx]) { r->active[idx] = 1; r->n_active_readers++; }
out:
    pthread_mutex_unlock(&r->m);
    return rc;
}

/*
 * res_read_value: Reads value and version counter under lease.
 * Asserts reader is registered active and no writer has entered.
 */
int res_read_value(Resource *r, int idx, const char *pid, int64_t *val, int *ver, char *err, size_t en) {
    int rc = 0;
    pthread_mutex_lock(&r->m);
    if (r->active_writer >= 0)
        VIOLATION("Invariant 1 Violated: Reader %s read while writer %s active!", pid, r->active_writer_id);
    if (!r->active[idx])
        VIOLATION("Process %s attempted reading without an active lease!", pid);
    *val = r->value;
    *ver = r->version;
out:
    pthread_mutex_unlock(&r->m);
    return rc;
}

/*
 * res_exit_read: Releases a reader's active lease and updates occupancy metrics.
 */
int res_exit_read(Resource *r, int idx, const char *pid, char *err, size_t en) {
    int rc = 0;
    pthread_mutex_lock(&r->m);
    if (!r->active[idx])
        VIOLATION("Reader %s exiting read lease but was not registered as active!", pid);
    r->active[idx] = 0;
    r->n_active_readers--;
    if (r->n_active_readers == 0 && r->active_writer < 0) {
        double now = mono_now();
        r->occupied_time += now - r->last_change;
        r->is_occupied = 0;
        r->last_change = now;
    }
out:
    pthread_mutex_unlock(&r->m);
    return rc;
}

/*
 * res_enter_write: Registers an admitted writer.
 * Invariant 1 & 2 Check: Asserts n_active_readers == 0 and active_writer < 0.
 */
int res_enter_write(Resource *r, int idx, const char *pid, char *err, size_t en) {
    int rc = 0;
    pthread_mutex_lock(&r->m);
    if (r->active_writer >= 0)
        VIOLATION("Invariant 2 Violated: Writer %s entered while writer %s is active!", pid, r->active_writer_id);
    if (r->n_active_readers > 0)
        VIOLATION("Invariant 1 Violated: Writer %s entered while %d reader(s) are active!", pid, r->n_active_readers);
    if (!r->is_occupied) { r->is_occupied = 1; r->last_change = mono_now(); }
    r->active_writer = idx;
    snprintf(r->active_writer_id, sizeof r->active_writer_id, "%s", pid);
out:
    pthread_mutex_unlock(&r->m);
    return rc;
}

static int64_t clamp_js(int64_t v) {
    if (v > JS_SAFE_MAX) return JS_SAFE_MAX;
    if (v < -JS_SAFE_MAX) return -JS_SAFE_MAX;
    return v;
}

/*
 * res_execute_write: Applies an arithmetic mutation to the shared register.
 * Atomically increments monotonic version counter (v1, v2, v3...).
 */
int res_execute_write(Resource *r, int idx, const char *pid, WOp op, int amount, int64_t *val, int *ver, char *err, size_t en) {
    int rc = 0;
    pthread_mutex_lock(&r->m);
    if (r->active_writer != idx)
        VIOLATION("Process %s attempted write without exclusive lease!", pid);
    if (r->n_active_readers > 0)
        VIOLATION("Invariant 1 Violated: Write occurred while %d readers were active!", r->n_active_readers);
    switch (op) {
        case WOP_ADD:      r->value = clamp_js(r->value + amount); break;
        case WOP_SUBTRACT: r->value = clamp_js(r->value - amount); break;
        case WOP_MULTIPLY: {
            int64_t out;
            if (__builtin_mul_overflow(r->value, (int64_t)amount, &out)) out = ((r->value < 0) ? -JS_SAFE_MAX : JS_SAFE_MAX);
            r->value = clamp_js(out);
            break;
        }
        case WOP_SET:      r->value = amount; break;
    }
    r->version++;
    *val = r->value;
    *ver = r->version;
out:
    pthread_mutex_unlock(&r->m);
    return rc;
}

/*
 * res_exit_write: Releases writer's exclusive lease and updates occupancy metrics.
 */
int res_exit_write(Resource *r, int idx, const char *pid, char *err, size_t en) {
    int rc = 0;
    pthread_mutex_lock(&r->m);
    if (r->active_writer != idx)
        VIOLATION("Writer %s exiting but active writer was %s!", pid, r->active_writer >= 0 ? r->active_writer_id : "none");
    r->active_writer = -1;
    r->active_writer_id[0] = '\0';
    if (r->n_active_readers == 0) {
        double now = mono_now();
        r->occupied_time += now - r->last_change;
        r->is_occupied = 0;
        r->last_change = now;
    }
out:
    pthread_mutex_unlock(&r->m);
    return rc;
}

void res_snapshot(Resource *r, int64_t *val, int *ver, int *n_readers, int *writer_idx) {
    pthread_mutex_lock(&r->m);
    if (val) *val = r->value;
    if (ver) *ver = r->version;
    if (n_readers) *n_readers = r->n_active_readers;
    if (writer_idx) *writer_idx = r->active_writer;
    pthread_mutex_unlock(&r->m);
}

double res_occupancy_pct(Resource *r, double total_elapsed_sec) {
    pthread_mutex_lock(&r->m);
    double occ = r->occupied_time;
    if (r->is_occupied) occ += mono_now() - r->last_change;
    pthread_mutex_unlock(&r->m);
    if (total_elapsed_sec <= 0.0001) return 0.0;
    double ratio = occ / total_elapsed_sec * 100.0;
    if (ratio < 0.0) ratio = 0.0;
    if (ratio > 100.0) ratio = 100.0;
    return ratio;
}

/* ======================= Strategies ======================= */

/* sem_wait that keeps honouring stop / pause (the C twin of _acquire_with_cancellation). */
static int acquire(sem_t *s, Flag *stop, Flag *pause) {
    while (!flag_is_set(stop)) {
        if (pause && !flag_is_set(pause)) {
            flag_wait(pause, 0.1);
            continue;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100 * 1000000L; /* 100 ms slices */
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        int r;
        do { r = sem_timedwait(s, &ts); } while (r == -1 && errno == EINTR);
        if (r == 0) return 1;
    }
    return 0;
}

/* =========================================================================
 *                  SYNCHRONIZATION STRATEGIES (ALGORITHMS)
 * =========================================================================
 *
 * Implements the three canonical Readers-Writers synchronization algorithms
 * using POSIX semaphores (sem_t) and mutexes (pthread_mutex_t).
 *
 * All wait operations use acquire() [sem_timedwait in 20ms cooperative slices]
 * to allow clean cancellation (stop) and cooperative pause/resume without
 * leaking semaphore permits or inducing deadlocks.
 */

/* Helper: Blocking wait on a POSIX semaphore with automatic retry on signal interrupt */
static void sem_lock(sem_t *s) { while (sem_wait(s) == -1 && errno == EINTR) {} }

/*
 * strat_init: Initializes all synchronization primitives for the given algorithm.
 *
 * Semaphores:
 *   - mutex:    Binary semaphore (1) protecting the shared read_count.
 *   - mutex_w:  Binary semaphore (1) protecting write_count (Writer Preference).
 *   - read_try: Gate semaphore (1) locked by queued writers to block incoming readers.
 *   - resource: Mutual exclusion semaphore (1) guarding the shared register (wrt).
 *   - order:    FIFO turnstile semaphore (1) ensuring first-come, first-served admission.
 */
void strat_init(Strategy *s, Algo kind) {
    memset(s, 0, sizeof *s);
    s->kind = kind;
    pthread_mutex_init(&s->state_lock, NULL);
    sem_init(&s->mutex, 0, 1);
    sem_init(&s->mutex_w, 0, 1);
    sem_init(&s->read_try, 0, 1);
    sem_init(&s->resource, 0, 1);
    sem_init(&s->order, 0, 1);
    atomic_init(&s->read_count, 0);
    atomic_init(&s->write_count, 0);
}

/*
 * strat_destroy: Deallocates and cleans up all POSIX semaphores and mutexes.
 */
void strat_destroy(Strategy *s) {
    sem_destroy(&s->mutex);
    sem_destroy(&s->mutex_w);
    sem_destroy(&s->read_try);
    sem_destroy(&s->resource);
    sem_destroy(&s->order);
    pthread_mutex_destroy(&s->state_lock);
}

/* Helper to atomically track the count of threads currently waiting in queue */
static void waiting_add(Strategy *s, int *counter, int d) {
    pthread_mutex_lock(&s->state_lock);
    *counter += d;
    pthread_mutex_unlock(&s->state_lock);
}

/*
 * =========================================================================
 *                         READER ADMISSION PROTOCOL
 * =========================================================================
 * strat_reader_enter:
 *   Called by an arriving reader thread requesting entry to the critical section.
 *   Returns 1 if admitted, 0 if cancelled/stopped.
 */
int strat_reader_enter(Strategy *s, Flag *stop, Flag *pause) {
    waiting_add(s, &s->waiting_readers, +1);
    int ok = 0;

    switch (s->kind) {

    /*
     * ---------------------------------------------------------------------
     * ALGORITHM 1: READER PREFERENCE (Courtois et al. 1971 - First Problem)
     * ---------------------------------------------------------------------
     * Logic:
     *   1. Acquire 'mutex' to protect access to 'read_count'.
     *   2. Increment 'read_count'.
     *   3. If this is the FIRST reader (read_count == 1), acquire 'resource'.
     *      This locks out all writers while at least one reader is active.
     *   4. Release 'mutex' so subsequent readers can arrive and read concurrently.
     * Consequence:
     *   High reader throughput, but continuous reader arrivals cause indefinite
     *   WRITER STARVATION.
     */
    case ALGO_READER_PREF:
        if (!acquire(&s->mutex, stop, pause)) break;
        if (atomic_fetch_add(&s->read_count, 1) + 1 == 1) {
            /* First reader claims the shared resource lock */
            if (!acquire(&s->resource, stop, pause)) {
                atomic_fetch_sub(&s->read_count, 1);
                sem_post(&s->mutex);
                break;
            }
        }
        sem_post(&s->mutex);
        ok = 1;
        break;

    /*
     * ---------------------------------------------------------------------
     * ALGORITHM 2: WRITER PREFERENCE (Courtois et al. 1971 - Second Problem)
     * ---------------------------------------------------------------------
     * Logic:
     *   1. Arriving readers must pass through the 'read_try' gate.
     *   2. If a writer has queued, it has already locked 'read_try', so
     *      new readers are blocked immediately before modifying read_count.
     *   3. Once past 'read_try', reader claims 'mutex' and increments read_count.
     *   4. First reader claims 'resource' (waiting for any active writer to finish).
     *   5. Release 'mutex' and 'read_try'.
     * Consequence:
     *   Writers gain priority. Continuous writer arrivals cause READER STARVATION.
     */
    case ALGO_WRITER_PREF:
        /* Reader must pass the writer-controlled admission gate */
        if (!acquire(&s->read_try, stop, pause)) break;
        if (!acquire(&s->mutex, stop, pause)) { sem_post(&s->read_try); break; }
        if (atomic_fetch_add(&s->read_count, 1) + 1 == 1) {
            /* First reader claims resource lock against idle writers */
            if (!acquire(&s->resource, stop, pause)) {
                atomic_fetch_sub(&s->read_count, 1);
                sem_post(&s->mutex);
                sem_post(&s->read_try);
                break;
            }
        }
        sem_post(&s->mutex);
        sem_post(&s->read_try); /* Re-open gate for fellow readers */
        ok = 1;
        break;

    /*
     * ---------------------------------------------------------------------
     * ALGORITHM 3: FAIR FIFO ADMISSION (Starvation-Free Turnstile)
     * ---------------------------------------------------------------------
     * Logic:
     *   1. Arriving thread (reader or writer) acquires 'order' turnstile.
     *   2. Reader increments 'read_count' under 'mutex'.
     *   3. First reader claims 'resource'.
     *   4. Reader immediately releases 'order' turnstile before reading.
     * Consequence:
     *   Consecutive readers read in parallel, but an arriving reader CANNOT
     *   jump ahead of an earlier queued writer at the turnstile.
     *   Guarantees ZERO STARVATION for both readers and writers.
     */
    case ALGO_FAIR_FIFO:
        /* Thread passes through FIFO turnstile in strict order of arrival */
        if (!acquire(&s->order, stop, pause)) break;
        if (!acquire(&s->mutex, stop, pause)) { sem_post(&s->order); break; }
        if (atomic_fetch_add(&s->read_count, 1) + 1 == 1) {
            /* First reader locks resource */
            if (!acquire(&s->resource, stop, pause)) {
                atomic_fetch_sub(&s->read_count, 1);
                sem_post(&s->mutex);
                sem_post(&s->order);
                break;
            }
        }
        sem_post(&s->mutex);
        sem_post(&s->order); /* Turnstile released for next waiting thread */
        ok = 1;
        break;
    }

    waiting_add(s, &s->waiting_readers, -1);
    return ok;
}

/*
 * =========================================================================
 *                          READER EXIT PROTOCOL
 * =========================================================================
 * strat_reader_exit:
 *   Called when a reader thread departs the critical section.
 *   Decrements 'read_count'. When the LAST reader departs (read_count == 0),
 *   it releases the 'resource' semaphore, allowing waiting writers to proceed.
 */
void strat_reader_exit(Strategy *s) {
    sem_lock(&s->mutex);
    if (atomic_fetch_sub(&s->read_count, 1) - 1 == 0) {
        sem_post(&s->resource); /* Last exiting reader unlocks the resource */
    }
    sem_post(&s->mutex);
}

/*
 * =========================================================================
 *                         WRITER ADMISSION PROTOCOL
 * =========================================================================
 * strat_writer_enter:
 *   Called by an arriving writer thread requesting exclusive access.
 *   Returns 1 if admitted, 0 if cancelled/stopped.
 */
int strat_writer_enter(Strategy *s, Flag *stop, Flag *pause) {
    waiting_add(s, &s->waiting_writers, +1);
    int ok = 0;

    switch (s->kind) {

    /*
     * ALGORITHM 1: READER PREFERENCE - WRITER ADMISSION
     *   Writer simply waits directly on 'resource'. If readers are active
     *   or continuously arriving, writer waits indefinitely.
     */
    case ALGO_READER_PREF:
        ok = acquire(&s->resource, stop, pause);
        break;

    /*
     * ALGORITHM 2: WRITER PREFERENCE - WRITER ADMISSION
     *   1. Under 'mutex_w', increment 'write_count'.
     *   2. If FIRST writer (write_count == 1), acquire 'read_try' gate.
     *      This locks out all future readers immediately.
     *   3. Acquire 'resource' to wait for any existing active readers to exit.
     */
    case ALGO_WRITER_PREF:
        if (!acquire(&s->mutex_w, stop, pause)) break;
        if (atomic_fetch_add(&s->write_count, 1) + 1 == 1) {
            /* First queued writer locks the reader gate */
            if (!acquire(&s->read_try, stop, pause)) {
                atomic_fetch_sub(&s->write_count, 1);
                sem_post(&s->mutex_w);
                break;
            }
        }
        sem_post(&s->mutex_w);
        if (!acquire(&s->resource, stop, pause)) {
            sem_lock(&s->mutex_w);
            if (atomic_fetch_sub(&s->write_count, 1) - 1 == 0) sem_post(&s->read_try);
            sem_post(&s->mutex_w);
            break;
        }
        ok = 1;
        break;

    /*
     * ALGORITHM 3: FAIR FIFO ADMISSION - WRITER ADMISSION
     *   1. Writer acquires 'order' turnstile.
     *   2. Writer acquires 'resource' (waiting for active readers/writers to exit).
     *   3. Releases 'order' turnstile so next thread in queue can advance.
     */
    case ALGO_FAIR_FIFO:
        if (!acquire(&s->order, stop, pause)) break;
        if (!acquire(&s->resource, stop, pause)) { sem_post(&s->order); break; }
        sem_post(&s->order); /* Pass turnstile to next thread */
        ok = 1;
        break;
    }

    waiting_add(s, &s->waiting_writers, -1);
    return ok;
}

/*
 * =========================================================================
 *                          WRITER EXIT PROTOCOL
 * =========================================================================
 * strat_writer_exit:
 *   Called when a writer thread finishes writing and departs.
 *   Releases the 'resource' lock. In Writer Preference, decrements
 *   'write_count'; if no more writers remain, re-opens 'read_try'.
 */
void strat_writer_exit(Strategy *s) {
    sem_post(&s->resource); /* Release exclusive lock on shared resource */
    if (s->kind == ALGO_WRITER_PREF) {
        sem_lock(&s->mutex_w);
        if (atomic_fetch_sub(&s->write_count, 1) - 1 == 0) {
            sem_post(&s->read_try); /* Last departing writer unlocks reader gate */
        }
        sem_post(&s->mutex_w);
    }
}

/*
 * strat_state: Captures an atomic snapshot of semaphore states, counters,
 * and current admission policy description for telemetry and UI visualization.
 */
void strat_state(Strategy *s, SemState *o) {
    memset(o, 0, sizeof *o);
    pthread_mutex_lock(&s->state_lock);
    o->waiting_readers = s->waiting_readers;
    o->waiting_writers = s->waiting_writers;
    pthread_mutex_unlock(&s->state_lock);
    o->read_count = atomic_load(&s->read_count);
    o->write_count = atomic_load(&s->write_count);
    switch (s->kind) {
    case ALGO_READER_PREF:
        o->write_count = 0;
        o->resource_locked = (o->read_count > 0 || o->waiting_writers > 0);
        o->admission_policy = "Readers share access. First reader locks out writers. Writers wait until read_count drops to 0. Arriving readers can starve writers.";
        break;
    case ALGO_WRITER_PREF:
        o->resource_locked = (o->read_count > 0 || o->write_count > 0);
        o->read_try_locked = (o->write_count > 0);
        o->admission_policy = "Writers have priority. When any writer queues, read_try is locked against incoming readers. Arriving writers can starve readers.";
        break;
    case ALGO_FAIR_FIFO:
        o->write_count = 0;
        o->resource_locked = (o->read_count > 0 || o->waiting_writers > 0);
        o->turnstile_busy = (o->waiting_readers + o->waiting_writers > 0);
        o->admission_policy = "FIFO Fair Turnstile. Processes are admitted strictly in order of request. Readers read concurrently if consecutive. Neither readers nor writers starve.";
        break;
    }
}

/* ======================= Engine ======================= */

void simconfig_default(SimConfig *c) {
    c->algorithm = ALGO_READER_PREF;
    c->reader_count = 6;
    c->writer_count = 2;
    c->read_duration_ms = 1200;
    c->write_duration_ms = 1800;
    c->initial_value = 100;
    c->writer_op = WOP_ADD;
    c->writer_amount = 10;
    c->max_duration_sec = 60;
    c->stagger_arrivals_ms = 200;
}

void simconfig_json(StrBuf *sb, const SimConfig *c) {
    sb_printf(sb,
        "{\"algorithm\":\"%s\",\"reader_count\":%d,\"writer_count\":%d,\"read_duration_ms\":%d,"
        "\"write_duration_ms\":%d,\"initial_value\":%d,\"writer_op\":\"%s\",\"writer_amount\":%d,"
        "\"max_duration_sec\":%d,\"stagger_arrivals_ms\":%d}",
        algo_name(c->algorithm), c->reader_count, c->writer_count, c->read_duration_ms,
        c->write_duration_ms, c->initial_value, wop_name(c->writer_op), c->writer_amount,
        c->max_duration_sec, c->stagger_arrivals_ms);
}

static void init_processes_locked(Engine *e) {
    double now = wall_now();
    e->nprocs = 0;
    for (int i = 1; i <= e->cfg.reader_count; i++) {
        Proc *p = &e->procs[e->nprocs++];
        memset(p, 0, sizeof *p);
        snprintf(p->id, sizeof p->id, "R%02d", i);
        p->type = PT_READER;
        p->state = PS_CREATED;
        p->created_at = now;
        p->duration_ms = (double)e->cfg.read_duration_ms;
    }
    for (int j = 1; j <= e->cfg.writer_count; j++) {
        Proc *p = &e->procs[e->nprocs++];
        memset(p, 0, sizeof *p);
        snprintf(p->id, sizeof p->id, "W%02d", j);
        p->type = PT_WRITER;
        p->state = PS_CREATED;
        p->created_at = now;
        p->duration_ms = (double)e->cfg.write_duration_ms;
        p->has_op = 1;
        p->op = e->cfg.writer_op;
        p->has_amount = 1;
        p->amount = e->cfg.writer_amount;
    }
}

static void make_sim_id(char *out, size_t n) {
    unsigned char b[4] = { 0 };
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        if (read(fd, b, sizeof b) != (ssize_t)sizeof b) memset(b, 0, sizeof b);
        close(fd);
    }
    if (!b[0] && !b[1] && !b[2] && !b[3]) {
        unsigned v = (unsigned)time(NULL) ^ (unsigned)getpid();
        memcpy(b, &v, 4);
    }
    snprintf(out, n, "%02x%02x%02x%02x", b[0], b[1], b[2], b[3]);
}

Engine *engine_create(const SimConfig *cfg, EventCb cb, void *ctx) {
    Engine *e = (Engine *)calloc(1, sizeof *e);
    if (!e) return NULL;
    make_sim_id(e->simulation_id, sizeof e->simulation_id);
    e->cfg = *cfg;
    e->cb = cb;
    e->cb_ctx = ctx;
    e->broadcast = 1;
    pthread_mutex_init(&e->lock, NULL);
    e->status = SS_IDLE;
    flag_init(&e->stop);
    flag_init(&e->pause);
    flag_set(&e->pause);
    strat_init(&e->strat, cfg->algorithm);
    res_init(&e->res, cfg->initial_value);
    e->capev = 256;
    e->events = (SimEvent *)malloc(sizeof(SimEvent) * (size_t)e->capev);
    init_processes_locked(e);
    return e;
}

SimStatus engine_status(Engine *e) {
    pthread_mutex_lock(&e->lock);
    SimStatus s = e->status;
    pthread_mutex_unlock(&e->lock);
    return s;
}

/* ---- events ---- */

void engine_event_json(StrBuf *sb, const SimEvent *ev) {
    sb_printf(sb, "{\"seq\":%d,\"timestamp_rel_ms\":", ev->seq);
    sb_json_dbl(sb, ev->rel_ms, 2);
    sb_puts(sb, ",\"timestamp_iso\":");
    sb_json_str(sb, ev->iso);
    sb_puts(sb, ",\"process_id\":");
    if (ev->has_pid) sb_json_str(sb, ev->pid); else sb_puts(sb, "null");
    sb_puts(sb, ",\"process_type\":");
    if (ev->ptype >= 0) sb_printf(sb, "\"%s\"", ptype_name((PType)ev->ptype)); else sb_puts(sb, "null");
    sb_puts(sb, ",\"event_type\":");
    sb_json_str(sb, ev->etype);
    sb_puts(sb, ",\"from_state\":");
    if (ev->from >= 0) sb_printf(sb, "\"%s\"", pstate_name((PState)ev->from)); else sb_puts(sb, "null");
    sb_puts(sb, ",\"to_state\":");
    if (ev->to >= 0) sb_printf(sb, "\"%s\"", pstate_name((PState)ev->to)); else sb_puts(sb, "null");
    sb_printf(sb, ",\"resource_value\":%lld,\"resource_version\":%d,\"active_readers\":%d,\"active_writers\":%d,"
                  "\"waiting_readers\":%d,\"waiting_writers\":%d,\"details\":",
              (long long)ev->value, ev->version, ev->active_readers, ev->active_writers,
              ev->waiting_readers, ev->waiting_writers);
    sb_json_str(sb, ev->details);
    sb_putc(sb, '}');
}

static void emit(Engine *e, const char *etype, int pidx, int from, int to, const char *fmt, ...) {
    SimEvent ev;
    memset(&ev, 0, sizeof ev);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ev.details, sizeof ev.details, fmt, ap);
    va_end(ap);
    snprintf(ev.etype, sizeof ev.etype, "%s", etype);
    ev.from = from;
    ev.to = to;
    iso_now(ev.iso, sizeof ev.iso);

    pthread_mutex_lock(&e->lock);
    ev.seq = ++e->seq;
    ev.rel_ms = round_to(e->start_mono > 0 ? (mono_now() - e->start_mono) * 1000.0 : 0.0, 2);
    int nr, widx;
    res_snapshot(&e->res, &ev.value, &ev.version, &nr, &widx);
    SemState ss;
    strat_state(&e->strat, &ss);
    ev.active_readers = nr;
    ev.active_writers = widx >= 0 ? 1 : 0;
    ev.waiting_readers = ss.waiting_readers;
    ev.waiting_writers = ss.waiting_writers;
    ev.ptype = -1;
    if (pidx >= 0 && pidx < e->nprocs) {
        ev.has_pid = 1;
        snprintf(ev.pid, sizeof ev.pid, "%s", e->procs[pidx].id);
        ev.ptype = (int)e->procs[pidx].type;
    }
    if (e->nev == e->capev) {
        int ncap = e->capev * 2;
        SimEvent *ne = (SimEvent *)realloc(e->events, sizeof(SimEvent) * (size_t)ncap);
        if (ne) { e->events = ne; e->capev = ncap; }
    }
    if (e->nev < e->capev) e->events[e->nev++] = ev;
    EventCb cb = e->broadcast ? e->cb : NULL;
    void *ctx = e->cb_ctx;
    pthread_mutex_unlock(&e->lock);

    if (cb) {
        StrBuf sb;
        sb_init(&sb);
        engine_event_json(&sb, &ev);
        cb(sb.data, sb.len, ctx);
        sb_free(&sb);
    }
}

/* ---- workers ---- */

typedef struct { Engine *e; int idx; double delay; } WorkerArg;

static void fmt2(char *buf, size_t n, double v) { fmt_py_float(buf, n, v, 2); }

/* cooperative arrival delay: honours stop + pause, in 50 ms slices */
static void arrival_delay(Engine *e, double delay) {
    double remaining = delay;
    while (remaining > 0 && !flag_is_set(&e->stop)) {
        if (!flag_is_set(&e->pause)) { flag_wait(&e->pause, 0.05); continue; }
        double slice = remaining < 0.05 ? remaining : 0.05;
        if (flag_wait(&e->stop, slice)) break;
        remaining -= slice;
    }
}

/* the 10-step critical-section body with cooperative pause (lease stays held while paused) */
static void run_slices(Engine *e, int idx, double total_sec) {
    const int steps = 10;
    double step_dur = total_sec / steps;
    int step = 1;
    while (step <= steps && !flag_is_set(&e->stop)) {
        if (!flag_is_set(&e->pause)) { flag_wait(&e->pause, 0.02); continue; }
        double slice_end = mono_now() + step_dur;
        int stopped = 0, paused = 0;
        while (mono_now() < slice_end) {
            double w = slice_end - mono_now();
            if (w > 0.02) w = 0.02;
            if (w < 0.001) w = 0.001;
            if (flag_wait(&e->stop, w)) { stopped = 1; break; }
            if (!flag_is_set(&e->pause)) { paused = 1; break; }
        }
        if (stopped) break;
        if (paused) continue;
        pthread_mutex_lock(&e->lock);
        e->procs[idx].progress_pct = round_to((double)step / steps * 100.0, 1);
        pthread_mutex_unlock(&e->lock);
        step++;
    }
}

static void proc_fail(Engine *e, int idx, PState from, const char *msg) {
    pthread_mutex_lock(&e->lock);
    Proc *p = &e->procs[idx];
    p->state = PS_ERROR;
    p->has_error = 1;
    snprintf(p->error, sizeof p->error, "%s", msg);
    p->has_completed = 1;
    p->completed_at = wall_now();
    pthread_mutex_unlock(&e->lock);
    emit(e, "INVARIANT_VIOLATION", idx, (int)from, (int)PS_ERROR, "%s", msg);
}

static void proc_cancelled(Engine *e, int idx, const char *who) {
    char id[16];
    pthread_mutex_lock(&e->lock);
    Proc *p = &e->procs[idx];
    p->state = PS_CANCELLED;
    p->has_completed = 1;
    p->completed_at = wall_now();
    snprintf(id, sizeof id, "%s", p->id);
    pthread_mutex_unlock(&e->lock);
    emit(e, "PROCESS_CANCELLED", idx, (int)PS_WAITING, (int)PS_CANCELLED, "%s %s was cancelled while waiting.", who, id);
}

static void *reader_worker(void *arg) {
    WorkerArg *wa = (WorkerArg *)arg;
    Engine *e = wa->e;
    int idx = wa->idx;
    double delay = wa->delay;
    free(wa);
    char id[16], err[200];
    snprintf(id, sizeof id, "%s", e->procs[idx].id);

    if (delay > 0) arrival_delay(e, delay);
    if (flag_is_set(&e->stop)) {
        pthread_mutex_lock(&e->lock);
        e->procs[idx].state = PS_CANCELLED;
        pthread_mutex_unlock(&e->lock);
        return NULL;
    }

    /* WAITING */
    pthread_mutex_lock(&e->lock);
    PState prev = e->procs[idx].state;
    e->procs[idx].state = PS_WAITING;
    e->procs[idx].has_requested = 1;
    e->procs[idx].requested_at = mono_now();
    pthread_mutex_unlock(&e->lock);
    emit(e, "PROCESS_REQUESTED", idx, (int)prev, (int)PS_WAITING, "Reader %s queued and requested read lease.", id);

    int admitted = strat_reader_enter(&e->strat, &e->stop, &e->pause);
    if (!admitted || flag_is_set(&e->stop)) {
        if (admitted) strat_reader_exit(&e->strat);
        proc_cancelled(e, idx, "Reader");
        return NULL;
    }

    /* READING */
    double admit = mono_now();
    double wait_ms;
    pthread_mutex_lock(&e->lock);
    Proc *p = &e->procs[idx];
    p->has_admitted = 1;
    p->admitted_at = admit;
    p->wait_time_ms = round_to((admit - (p->has_requested ? p->requested_at : admit)) * 1000.0, 2);
    wait_ms = p->wait_time_ms;
    p->state = PS_READING;
    pthread_mutex_unlock(&e->lock);

    int64_t val = 0;
    int ver = 0;
    if (res_enter_read(&e->res, idx, id, err, sizeof err) != 0 ||
        res_read_value(&e->res, idx, id, &val, &ver, err, sizeof err) != 0) {
        strat_reader_exit(&e->strat);
        proc_fail(e, idx, PS_WAITING, err);
        return NULL;
    }
    char ws[32];
    fmt2(ws, sizeof ws, wait_ms);
    emit(e, "READ_ACQUIRED", idx, (int)PS_WAITING, (int)PS_READING,
         "Reader %s admitted (wait: %sms). Read current value=%lld, ver=%d.", id, ws, (long long)val, ver);

    run_slices(e, idx, e->cfg.read_duration_ms / 1000.0);

    if (res_exit_read(&e->res, idx, id, err, sizeof err) != 0) {
        strat_reader_exit(&e->strat);
        proc_fail(e, idx, PS_READING, err);
        return NULL;
    }
    strat_reader_exit(&e->strat);

    double comp = mono_now();
    char ds[32];
    pthread_mutex_lock(&e->lock);
    p = &e->procs[idx];
    p->state = PS_COMPLETED;
    p->has_completed = 1;
    p->completed_at = wall_now();
    p->duration_ms = round_to((comp - admit) * 1000.0, 2);
    p->progress_pct = 100.0;
    fmt2(ds, sizeof ds, p->duration_ms);
    pthread_mutex_unlock(&e->lock);
    emit(e, "READ_RELEASED", idx, (int)PS_READING, (int)PS_COMPLETED, "Reader %s released read lease after %sms.", id, ds);
    return NULL;
}

static void *writer_worker(void *arg) {
    WorkerArg *wa = (WorkerArg *)arg;
    Engine *e = wa->e;
    int idx = wa->idx;
    double delay = wa->delay;
    free(wa);
    char id[16], err[200];
    snprintf(id, sizeof id, "%s", e->procs[idx].id);

    if (delay > 0) arrival_delay(e, delay);
    if (flag_is_set(&e->stop)) {
        pthread_mutex_lock(&e->lock);
        e->procs[idx].state = PS_CANCELLED;
        pthread_mutex_unlock(&e->lock);
        return NULL;
    }

    pthread_mutex_lock(&e->lock);
    PState prev = e->procs[idx].state;
    e->procs[idx].state = PS_WAITING;
    e->procs[idx].has_requested = 1;
    e->procs[idx].requested_at = mono_now();
    pthread_mutex_unlock(&e->lock);
    emit(e, "PROCESS_REQUESTED", idx, (int)prev, (int)PS_WAITING, "Writer %s queued for exclusive write lease.", id);

    int admitted = strat_writer_enter(&e->strat, &e->stop, &e->pause);
    if (!admitted || flag_is_set(&e->stop)) {
        if (admitted) strat_writer_exit(&e->strat);
        proc_cancelled(e, idx, "Writer");
        return NULL;
    }

    double admit = mono_now();
    double wait_ms;
    pthread_mutex_lock(&e->lock);
    Proc *p = &e->procs[idx];
    p->has_admitted = 1;
    p->admitted_at = admit;
    p->wait_time_ms = round_to((admit - (p->has_requested ? p->requested_at : admit)) * 1000.0, 2);
    wait_ms = p->wait_time_ms;
    p->state = PS_WRITING;
    pthread_mutex_unlock(&e->lock);

    if (res_enter_write(&e->res, idx, id, err, sizeof err) != 0) {
        strat_writer_exit(&e->strat);
        proc_fail(e, idx, PS_WAITING, err);
        return NULL;
    }
    char ws[32];
    fmt2(ws, sizeof ws, wait_ms);
    emit(e, "WRITE_ACQUIRED", idx, (int)PS_WAITING, (int)PS_WRITING, "Writer %s acquired exclusive lock (wait: %sms).", id, ws);

    run_slices(e, idx, e->cfg.write_duration_ms / 1000.0);

    int64_t nv = 0;
    int nver = 0;
    if (res_execute_write(&e->res, idx, id, e->cfg.writer_op, e->cfg.writer_amount, &nv, &nver, err, sizeof err) != 0) {
        res_exit_write(&e->res, idx, id, NULL, 0);
        strat_writer_exit(&e->strat);
        proc_fail(e, idx, PS_WRITING, err);
        return NULL;
    }
    if (res_exit_write(&e->res, idx, id, err, sizeof err) != 0) {
        strat_writer_exit(&e->strat);
        proc_fail(e, idx, PS_WRITING, err);
        return NULL;
    }
    strat_writer_exit(&e->strat);

    double comp = mono_now();
    char ds[32];
    pthread_mutex_lock(&e->lock);
    p = &e->procs[idx];
    p->state = PS_COMPLETED;
    p->has_completed = 1;
    p->completed_at = wall_now();
    p->duration_ms = round_to((comp - admit) * 1000.0, 2);
    p->progress_pct = 100.0;
    fmt2(ds, sizeof ds, p->duration_ms);
    pthread_mutex_unlock(&e->lock);
    emit(e, "WRITE_COMMITTED", idx, (int)PS_WRITING, (int)PS_COMPLETED,
         "Writer %s committed %s %d -> val=%lld, ver=%d after %sms.", id, wop_name(e->cfg.writer_op),
         e->cfg.writer_amount, (long long)nv, nver, ds);
    return NULL;
}

/* ---- supervisor ---- */

static void *supervisor_main(void *arg) {
    Engine *e = (Engine *)arg;
    double max_duration = (double)e->cfg.max_duration_sec;
    while (!flag_wait(&e->stop, 0.1)) {
        pthread_mutex_lock(&e->lock);
        SimStatus st = e->status;
        double start = e->start_mono, paused = e->total_paused_sec, pstart = e->pause_start_mono;
        pthread_mutex_unlock(&e->lock);

        double now = mono_now();
        double active_pause = (st == SS_PAUSED) ? now - pstart : 0.0;
        double elapsed = start > 0 ? (now - start) - paused - active_pause : 0.0;
        if (elapsed >= max_duration) {
            char ms[32];
            fmt_py_float(ms, sizeof ms, max_duration, 1);
            emit(e, "TIMEOUT", -1, -1, -1, "Simulation hit maximum limit of %ss.", ms);
            engine_stop(e);
            break;
        }

        int all_done = 1;
        pthread_mutex_lock(&e->lock);
        for (int i = 0; i < e->nprocs; i++) {
            PState s = e->procs[i].state;
            if (s != PS_COMPLETED && s != PS_CANCELLED && s != PS_ERROR) { all_done = 0; break; }
        }
        if (all_done) {
            e->end_mono = mono_now();
            if (e->status == SS_RUNNING || e->status == SS_PAUSED) e->status = SS_COMPLETED;
        }
        pthread_mutex_unlock(&e->lock);
        if (all_done) {
            emit(e, "SIMULATION_COMPLETED", -1, -1, -1, "All concurrent processes completed execution.");
            break;
        }
    }
    return NULL;
}

/* ---- lifecycle ---- */

static void join_all(Engine *e) {
    for (int i = 0; i < e->nthreads; i++) pthread_join(e->threads[i], NULL);
    e->nthreads = 0;
    if (e->has_supervisor) { pthread_join(e->supervisor, NULL); e->has_supervisor = 0; }
}

void engine_join(Engine *e) { join_all(e); }

void engine_start(Engine *e) {
    pthread_mutex_lock(&e->lock);
    if (e->status == SS_RUNNING || e->status == SS_PAUSED) { pthread_mutex_unlock(&e->lock); return; }
    pthread_mutex_unlock(&e->lock);
    join_all(e); /* collect threads of a previous (finished) run, if any */

    pthread_mutex_lock(&e->lock);
    e->status = SS_RUNNING;
    flag_clear(&e->stop);
    flag_set(&e->pause);
    e->pause_start_mono = 0.0;
    e->total_paused_sec = 0.0;
    e->start_mono = mono_now();
    e->end_mono = 0.0;
    pthread_mutex_unlock(&e->lock);

    emit(e, "SIMULATION_STARTED", -1, -1, -1, "Simulation started using %s algorithm.", algo_name(e->cfg.algorithm));

    /* arrival order: up to 2 readers, then 1 writer, repeated (same as the original engine) */
    int order[MAX_PROCS], n = 0;
    int ri = 0, wi = e->cfg.reader_count;
    int rend = e->cfg.reader_count, wend = e->nprocs;
    while (ri < rend || wi < wend) {
        for (int k = 0; k < 2; k++)
            if (ri < rend) order[n++] = ri++;
        if (wi < wend) order[n++] = wi++;
    }

    for (int i = 0; i < n; i++) {
        WorkerArg *wa = (WorkerArg *)malloc(sizeof *wa);
        if (!wa) continue;
        wa->e = e;
        wa->idx = order[i];
        wa->delay = (i * (double)e->cfg.stagger_arrivals_ms) / 1000.0;
        void *(*fn)(void *) = (e->procs[order[i]].type == PT_READER) ? reader_worker : writer_worker;
        if (pthread_create(&e->threads[e->nthreads], NULL, fn, wa) == 0) {
            e->nthreads++;
        } else {
            free(wa);
            pthread_mutex_lock(&e->lock);
            e->procs[order[i]].state = PS_ERROR;
            e->procs[order[i]].has_error = 1;
            snprintf(e->procs[order[i]].error, sizeof e->procs[order[i]].error, "thread creation failed");
            pthread_mutex_unlock(&e->lock);
        }
    }
    if (pthread_create(&e->supervisor, NULL, supervisor_main, e) == 0) e->has_supervisor = 1;
}

void engine_pause(Engine *e) {
    pthread_mutex_lock(&e->lock);
    if (e->status != SS_RUNNING) { pthread_mutex_unlock(&e->lock); return; }
    e->status = SS_PAUSED;
    flag_clear(&e->pause);
    e->pause_start_mono = mono_now();
    pthread_mutex_unlock(&e->lock);
    emit(e, "SIMULATION_PAUSED", -1, -1, -1, "Simulation paused. Critical sections and queued threads held safely without deadlocks.");
}

void engine_resume(Engine *e) {
    pthread_mutex_lock(&e->lock);
    if (e->status != SS_PAUSED) { pthread_mutex_unlock(&e->lock); return; }
    e->status = SS_RUNNING;
    double delta = mono_now() - e->pause_start_mono;
    e->total_paused_sec += delta;
    flag_set(&e->pause);
    pthread_mutex_unlock(&e->lock);
    char ds[32];
    fmt_py_float(ds, sizeof ds, delta * 1000.0, 1);
    emit(e, "SIMULATION_RESUMED", -1, -1, -1, "Simulation resumed after %sms pause.", ds);
}

void engine_stop(Engine *e) {
    pthread_mutex_lock(&e->lock);
    if (e->status == SS_COMPLETED || e->status == SS_CANCELLED) { pthread_mutex_unlock(&e->lock); return; }
    if (e->status == SS_PAUSED) e->total_paused_sec += mono_now() - e->pause_start_mono;
    e->status = SS_CANCELLED;
    flag_set(&e->stop);
    flag_set(&e->pause); /* unblock paused threads so they notice stop */
    e->end_mono = mono_now();
    pthread_mutex_unlock(&e->lock);

    emit(e, "SIMULATION_STOPPED", -1, -1, -1, "Stop requested. Gracefully signalling worker threads.");

    pthread_mutex_lock(&e->lock);
    for (int i = 0; i < e->nprocs; i++) {
        Proc *p = &e->procs[i];
        if (p->state == PS_CREATED || p->state == PS_WAITING) {
            p->state = PS_CANCELLED;
            p->has_completed = 1;
            p->completed_at = wall_now();
        }
    }
    pthread_mutex_unlock(&e->lock);
}

void engine_reset(Engine *e) {
    engine_stop(e);
    join_all(e);

    pthread_mutex_lock(&e->lock);
    e->nev = 0;
    e->seq = 0;
    e->status = SS_IDLE;
    flag_clear(&e->stop);
    flag_set(&e->pause);
    e->pause_start_mono = 0.0;
    e->total_paused_sec = 0.0;
    e->start_mono = 0.0;
    e->end_mono = 0.0;
    res_destroy(&e->res);
    res_init(&e->res, e->cfg.initial_value);
    strat_destroy(&e->strat);
    strat_init(&e->strat, e->cfg.algorithm);
    init_processes_locked(e);
    pthread_mutex_unlock(&e->lock);

    emit(e, "SIMULATION_RESET", -1, -1, -1, "Simulator reset to initial configuration.");
}

void engine_destroy(Engine *e) {
    if (!e) return;
    e->broadcast = 0;
    engine_stop(e);
    join_all(e);
    free(e->events);
    strat_destroy(&e->strat);
    res_destroy(&e->res);
    flag_destroy(&e->stop);
    flag_destroy(&e->pause);
    pthread_mutex_destroy(&e->lock);
    free(e);
}

/* ---- metrics ---- */

static double effective_elapsed_locked(Engine *e) {
    if (e->start_mono <= 0) return 0.001;
    double now = mono_now();
    int terminal = (e->status == SS_COMPLETED || e->status == SS_CANCELLED) && e->end_mono > 0;
    double ref = terminal ? e->end_mono : now;
    double active_pause = (e->status == SS_PAUSED) ? now - e->pause_start_mono : 0.0;
    double el = ref - e->start_mono - e->total_paused_sec - active_pause;
    return el < 0.001 ? 0.001 : el;
}

static void metrics_locked(Engine *e, Metrics *m) {
    memset(m, 0, sizeof *m);
    double elapsed = effective_elapsed_locked(e);
    double sum_r = 0, sum_w = 0, max_r = 0, max_w = 0;
    int cr = 0, cw = 0;
    for (int i = 0; i < e->nprocs; i++) {
        Proc *p = &e->procs[i];
        if (p->type == PT_READER) {
            m->total_readers++;
            if (p->state == PS_COMPLETED) { cr++; sum_r += p->wait_time_ms; if (p->wait_time_ms > max_r) max_r = p->wait_time_ms; }
        } else {
            m->total_writers++;
            if (p->state == PS_COMPLETED) { cw++; sum_w += p->wait_time_ms; if (p->wait_time_ms > max_w) max_w = p->wait_time_ms; }
        }
    }
    double avg_r = cr ? sum_r / cr : 0.0, avg_w = cw ? sum_w / cw : 0.0;
    int total_ops = cr + cw;
    double throughput = elapsed > 0.1 ? total_ops / elapsed : 0.0;
    double occ = res_occupancy_pct(&e->res, elapsed);

    if (avg_r > 0 && max_w > avg_r * 3.5 && cr >= 3) {
        m->starvation_warning = 1;
        char a[32], b[32];
        fmt_py_float(a, sizeof a, round_to(max_w, 1), 1);
        fmt_py_float(b, sizeof b, round_to(avg_r, 1), 1);
        snprintf(m->starvation_details, sizeof m->starvation_details,
                 "Writers experienced prolonged starvation (%sms max wait vs %sms reader avg).", a, b);
    } else if (avg_w > 0 && max_r > avg_w * 3.5 && cw >= 2) {
        m->starvation_warning = 1;
        char a[32], b[32];
        fmt_py_float(a, sizeof a, round_to(max_r, 1), 1);
        fmt_py_float(b, sizeof b, round_to(avg_w, 1), 1);
        snprintf(m->starvation_details, sizeof m->starvation_details,
                 "Readers experienced prolonged starvation (%sms max wait vs %sms writer avg).", a, b);
    }

    m->completed_reads = cr;
    m->completed_writes = cw;
    m->avg_reader_wait_ms = round_to(avg_r, 2);
    m->avg_writer_wait_ms = round_to(avg_w, 2);
    m->max_reader_wait_ms = round_to(max_r, 2);
    m->max_writer_wait_ms = round_to(max_w, 2);
    m->total_elapsed_ms = round_to(elapsed * 1000.0, 2);
    m->throughput_ops_per_sec = round_to(throughput, 2);
    m->resource_occupancy_pct = round_to(occ, 2);
}

void engine_metrics(Engine *e, Metrics *out) {
    pthread_mutex_lock(&e->lock);
    metrics_locked(e, out);
    pthread_mutex_unlock(&e->lock);
}

void engine_metrics_json(StrBuf *sb, const Metrics *m) {
    sb_printf(sb, "{\"total_readers\":%d,\"total_writers\":%d,\"completed_reads\":%d,\"completed_writes\":%d,",
              m->total_readers, m->total_writers, m->completed_reads, m->completed_writes);
    sb_puts(sb, "\"avg_reader_wait_ms\":");   sb_json_dbl(sb, m->avg_reader_wait_ms, 2);
    sb_puts(sb, ",\"avg_writer_wait_ms\":");  sb_json_dbl(sb, m->avg_writer_wait_ms, 2);
    sb_puts(sb, ",\"max_reader_wait_ms\":");  sb_json_dbl(sb, m->max_reader_wait_ms, 2);
    sb_puts(sb, ",\"max_writer_wait_ms\":");  sb_json_dbl(sb, m->max_writer_wait_ms, 2);
    sb_puts(sb, ",\"total_elapsed_ms\":");    sb_json_dbl(sb, m->total_elapsed_ms, 2);
    sb_puts(sb, ",\"throughput_ops_per_sec\":"); sb_json_dbl(sb, m->throughput_ops_per_sec, 2);
    sb_puts(sb, ",\"resource_occupancy_pct\":"); sb_json_dbl(sb, m->resource_occupancy_pct, 2);
    sb_printf(sb, ",\"starvation_warning\":%s,\"starvation_details\":", m->starvation_warning ? "true" : "false");
    if (m->starvation_warning) sb_json_str(sb, m->starvation_details); else sb_puts(sb, "null");
    sb_putc(sb, '}');
}

void engine_summary(Engine *e, int64_t *value, int *version, int *total_events) {
    res_snapshot(&e->res, value, version, NULL, NULL);
    pthread_mutex_lock(&e->lock);
    if (total_events) *total_events = e->nev;
    pthread_mutex_unlock(&e->lock);
}

/* ---- JSON state ---- */

static void proc_json(StrBuf *sb, const Proc *p) {
    sb_puts(sb, "{\"id\":"); sb_json_str(sb, p->id);
    sb_printf(sb, ",\"process_type\":\"%s\",\"state\":\"%s\",\"created_at\":", ptype_name(p->type), pstate_name(p->state));
    sb_json_dbl(sb, p->created_at, 6);
    sb_puts(sb, ",\"requested_at\":");
    if (p->has_requested) sb_json_dbl(sb, p->requested_at, 6); else sb_puts(sb, "null");
    sb_puts(sb, ",\"admitted_at\":");
    if (p->has_admitted) sb_json_dbl(sb, p->admitted_at, 6); else sb_puts(sb, "null");
    sb_puts(sb, ",\"completed_at\":");
    if (p->has_completed) sb_json_dbl(sb, p->completed_at, 6); else sb_puts(sb, "null");
    sb_puts(sb, ",\"wait_time_ms\":");  sb_json_dbl(sb, p->wait_time_ms, 2);
    sb_puts(sb, ",\"duration_ms\":");   sb_json_dbl(sb, p->duration_ms, 2);
    sb_puts(sb, ",\"progress_pct\":");  sb_json_dbl(sb, p->progress_pct, 1);
    sb_puts(sb, ",\"operation\":");
    if (p->has_op) sb_printf(sb, "\"%s\"", wop_name(p->op)); else sb_puts(sb, "null");
    sb_puts(sb, ",\"operation_amount\":");
    if (p->has_amount) sb_printf(sb, "%d", p->amount); else sb_puts(sb, "null");
    sb_puts(sb, ",\"error\":");
    if (p->has_error) sb_json_str(sb, p->error); else sb_puts(sb, "null");
    sb_putc(sb, '}');
}

static void semstate_json(StrBuf *sb, const SemState *s) {
    sb_printf(sb, "{\"read_count\":%d,\"write_count\":%d,\"waiting_readers\":%d,\"waiting_writers\":%d,"
                  "\"resource_locked\":%s,\"read_try_locked\":%s,\"turnstile_busy\":%s,\"admission_policy\":",
              s->read_count, s->write_count, s->waiting_readers, s->waiting_writers,
              s->resource_locked ? "true" : "false", s->read_try_locked ? "true" : "false",
              s->turnstile_busy ? "true" : "false");
    sb_json_str(sb, s->admission_policy);
    sb_putc(sb, '}');
}

void engine_state_json(Engine *e, StrBuf *sb) {
    pthread_mutex_lock(&e->lock);
    int64_t val;
    int ver, nr, widx;
    res_snapshot(&e->res, &val, &ver, &nr, &widx);
    SemState ss;
    strat_state(&e->strat, &ss);
    Metrics m;
    metrics_locked(e, &m);

    char occ[64];
    if (widx >= 0) snprintf(occ, sizeof occ, "EXCLUSIVE WRITE (%s)", e->procs[widx].id);
    else if (nr > 0) snprintf(occ, sizeof occ, "SHARED READ (%d readers)", nr);
    else snprintf(occ, sizeof occ, "IDLE");

    sb_printf(sb, "{\"simulation_id\":\"%s\",\"status\":\"%s\",\"algorithm\":\"%s\",\"config\":",
              e->simulation_id, status_name(e->status), algo_name(e->cfg.algorithm));
    simconfig_json(sb, &e->cfg);
    sb_printf(sb, ",\"resource_value\":%lld,\"resource_version\":%d,\"resource_occupancy\":", (long long)val, ver);
    sb_json_str(sb, occ);

    sb_puts(sb, ",\"active_reader_ids\":[");
    int first = 1;
    for (int i = 0; i < e->cfg.reader_count && i < e->nprocs; i++) {
        pthread_mutex_lock(&e->res.m);
        int act = e->res.active[i];
        pthread_mutex_unlock(&e->res.m);
        if (!act) continue;
        if (!first) sb_putc(sb, ',');
        first = 0;
        sb_json_str(sb, e->procs[i].id);
    }
    sb_puts(sb, "],\"active_writer_id\":");
    if (widx >= 0) sb_json_str(sb, e->procs[widx].id); else sb_puts(sb, "null");

    sb_puts(sb, ",\"processes\":[");
    for (int i = 0; i < e->nprocs; i++) {
        if (i) sb_putc(sb, ',');
        proc_json(sb, &e->procs[i]);
    }
    sb_puts(sb, "],\"semaphores\":");
    semstate_json(sb, &ss);
    sb_puts(sb, ",\"metrics\":");
    engine_metrics_json(sb, &m);
    sb_puts(sb, ",\"recent_events\":[");
    int start = e->nev > 30 ? e->nev - 30 : 0;
    for (int i = start; i < e->nev; i++) {
        if (i > start) sb_putc(sb, ',');
        engine_event_json(sb, &e->events[i]);
    }
    sb_printf(sb, "],\"total_events\":%d}", e->nev);
    pthread_mutex_unlock(&e->lock);
}

void engine_results_json(Engine *e, StrBuf *sb) {
    pthread_mutex_lock(&e->lock);
    int64_t val;
    int ver;
    res_snapshot(&e->res, &val, &ver, NULL, NULL);
    Metrics m;
    metrics_locked(e, &m);
    sb_printf(sb, "{\"simulation_id\":\"%s\",\"status\":\"%s\",\"algorithm\":\"%s\",\"config\":",
              e->simulation_id, status_name(e->status), algo_name(e->cfg.algorithm));
    simconfig_json(sb, &e->cfg);
    sb_printf(sb, ",\"final_resource_value\":%lld,\"final_resource_version\":%d,\"metrics\":", (long long)val, ver);
    engine_metrics_json(sb, &m);
    sb_puts(sb, ",\"processes\":[");
    for (int i = 0; i < e->nprocs; i++) {
        if (i) sb_putc(sb, ',');
        proc_json(sb, &e->procs[i]);
    }
    sb_printf(sb, "],\"total_events\":%d}", e->nev);
    pthread_mutex_unlock(&e->lock);
}

void engine_events_json(Engine *e, StrBuf *sb, int min_seq, const char *etype, int ptype, int limit) {
    pthread_mutex_lock(&e->lock);
    sb_putc(sb, '[');
    int emitted = 0;
    for (int i = 0; i < e->nev && emitted < limit; i++) {
        const SimEvent *ev = &e->events[i];
        if (ev->seq <= min_seq) continue;
        if (etype && strcmp(ev->etype, etype) != 0) continue;
        if (ptype >= 0 && ev->ptype != ptype) continue;
        if (emitted) sb_putc(sb, ',');
        engine_event_json(sb, ev);
        emitted++;
    }
    sb_putc(sb, ']');
    pthread_mutex_unlock(&e->lock);
}

void engine_events_csv(Engine *e, StrBuf *sb) {
    pthread_mutex_lock(&e->lock);
    if (e->nev == 0) {
        sb_puts(sb, "seq,timestamp_rel_ms,event_type,process_id,details\r\n");
    } else {
        sb_puts(sb, "seq,timestamp_rel_ms,timestamp_iso,process_id,process_type,event_type,from_state,to_state,"
                    "resource_value,resource_version,active_readers,active_writers,waiting_readers,waiting_writers,details\r\n");
        for (int i = 0; i < e->nev; i++) {
            const SimEvent *ev = &e->events[i];
            char t[32];
            fmt_py_float(t, sizeof t, ev->rel_ms, 2);
            sb_printf(sb, "%d,%s,%s,", ev->seq, t, ev->iso);
            if (ev->has_pid) sb_csv_field(sb, ev->pid);
            sb_putc(sb, ',');
            if (ev->ptype >= 0) sb_puts(sb, ptype_name((PType)ev->ptype));
            sb_putc(sb, ',');
            sb_csv_field(sb, ev->etype);
            sb_putc(sb, ',');
            if (ev->from >= 0) sb_puts(sb, pstate_name((PState)ev->from));
            sb_putc(sb, ',');
            if (ev->to >= 0) sb_puts(sb, pstate_name((PState)ev->to));
            sb_printf(sb, ",%lld,%d,%d,%d,%d,%d,", (long long)ev->value, ev->version, ev->active_readers,
                      ev->active_writers, ev->waiting_readers, ev->waiting_writers);
            sb_csv_field(sb, ev->details);
            sb_puts(sb, "\r\n");
        }
    }
    pthread_mutex_unlock(&e->lock);
}

void engine_events_after(Engine *e, int last_seq, StrBuf *sb) {
    pthread_mutex_lock(&e->lock);
    for (int i = 0; i < e->nev; i++) {
        if (e->events[i].seq <= last_seq) continue;
        engine_event_json(sb, &e->events[i]);
        sb_putc(sb, '\n');
    }
    pthread_mutex_unlock(&e->lock);
}
