/*
 * test_sync.c - concurrency / mutual-exclusion / fairness tests for the C backend.
 * C counterpart of the original tests/test_synchronization.py.
 *
 *   make test      (or: make build/test_sync && ./build/test_sync)
 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/sync.h"
#include "../src/util.h"

static int g_failed = 0, g_run = 0;
#define CHECK(cond) do { if (!(cond)) { printf("    FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failed++; return; } } while (0)
#define RUN(fn) do { g_run++; int before = g_failed; fn(); printf("%s %s\n", g_failed == before ? "PASS" : "FAIL", #fn); fflush(stdout); } while (0)

static void msleep(int ms) { usleep((useconds_t)ms * 1000); }

/* ---------- helpers ---------- */
typedef struct {
    Strategy *s;
    Flag *stop, *pause;
    int is_writer;
    volatile int entered;
    volatile int order;     /* position in global admission order */
    int *counter;
    pthread_mutex_t *cm;
} Actor;

static void *actor_main(void *a_) {
    Actor *a = (Actor *)a_;
    int ok = a->is_writer ? strat_writer_enter(a->s, a->stop, a->pause)
                          : strat_reader_enter(a->s, a->stop, a->pause);
    if (ok) {
        pthread_mutex_lock(a->cm);
        a->order = ++(*a->counter);
        pthread_mutex_unlock(a->cm);
        a->entered = 1;
    }
    return NULL;
}

static pthread_t start_actor(Actor *a, Strategy *s, Flag *stop, int is_writer, int *counter, pthread_mutex_t *cm) {
    memset(a, 0, sizeof *a);
    a->s = s; a->stop = stop; a->pause = NULL; a->is_writer = is_writer; a->counter = counter; a->cm = cm;
    pthread_t t;
    pthread_create(&t, NULL, actor_main, a);
    return t;
}

/* ---------- utilities ---------- */
static void test_websocket_accept_vector(void) {
    /* RFC 6455 section 1.3 example */
    const char *in = "dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t d[20];
    char out[64];
    sha1((const uint8_t *)in, strlen(in), d);
    base64_encode(d, 20, out);
    CHECK(strcmp(out, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);
}

static void test_json_object_reader(void) {
    JObject o;
    const char *ok = "{\"algorithm\":\"fair_fifo\",\"reader_count\":12,\"x\":{\"a\":[1,2,{\"b\":null}]},\"f\":1.5,\"s\":\"a\\u00e9\\n\",\"t\":true}";
    CHECK(json_parse_object(ok, strlen(ok), &o) == 0);
    CHECK(jobj_get(&o, "reader_count")->num == 12);
    CHECK(strcmp(jobj_get(&o, "algorithm")->str, "fair_fifo") == 0);
    CHECK(jobj_get(&o, "x")->type == JV_OTHER);
    CHECK(jobj_get(&o, "f")->num == 1.5);
    CHECK(strcmp(jobj_get(&o, "s")->str, "a\xc3\xa9\n") == 0);
    CHECK(jobj_get(&o, "t")->b == 1);
    CHECK(json_parse_object("{", 1, &o) == 1);
    CHECK(json_parse_object("{\"a\":}", 6, &o) == 1);
    CHECK(json_parse_object("{\"a\":1} x", 9, &o) == 1);
    CHECK(json_parse_object("[1,2]", 5, &o) == 2);
    CHECK(json_parse_object("", 0, &o) == 1);
}

static void test_python_style_float_format(void) {
    char b[32];
    fmt_py_float(b, sizeof b, 200.0, 2); CHECK(strcmp(b, "200.0") == 0);
    fmt_py_float(b, sizeof b, 0.12, 2);  CHECK(strcmp(b, "0.12") == 0);
    fmt_py_float(b, sizeof b, 15.5, 2);  CHECK(strcmp(b, "15.5") == 0);
    fmt_py_float(b, sizeof b, 1200.0, 1);CHECK(strcmp(b, "1200.0") == 0);
}

/* ---------- Resource invariants ---------- */
static void test_shared_resource_invariants(void) {
    Resource r;
    char err[200];
    int64_t v; int ver;
    res_init(&r, 50);

    /* Invariant 1: cannot read while a writer is active */
    CHECK(res_enter_write(&r, 10, "W01", err, sizeof err) == 0);
    CHECK(res_enter_read(&r, 0, "R01", err, sizeof err) == -1);
    CHECK(res_exit_write(&r, 10, "W01", err, sizeof err) == 0);

    /* Invariant 2: cannot write while readers are active */
    CHECK(res_enter_read(&r, 0, "R01", err, sizeof err) == 0);
    CHECK(res_enter_write(&r, 10, "W01", err, sizeof err) == -1);
    CHECK(res_exit_read(&r, 0, "R01", err, sizeof err) == 0);

    /* Invariant 3: single writer */
    CHECK(res_enter_write(&r, 10, "W01", err, sizeof err) == 0);
    CHECK(res_enter_write(&r, 11, "W02", err, sizeof err) == -1);
    CHECK(res_exit_write(&r, 10, "W01", err, sizeof err) == 0);

    /* Invariant 4: monotonic versioning */
    CHECK(res_enter_write(&r, 10, "W01", err, sizeof err) == 0);
    CHECK(res_execute_write(&r, 10, "W01", WOP_ADD, 25, &v, &ver, err, sizeof err) == 0);
    CHECK(v == 75 && ver == 1);
    CHECK(res_exit_write(&r, 10, "W01", err, sizeof err) == 0);
    CHECK(res_enter_write(&r, 11, "W02", err, sizeof err) == 0);
    CHECK(res_execute_write(&r, 11, "W02", WOP_MULTIPLY, 2, &v, &ver, err, sizeof err) == 0);
    CHECK(v == 150 && ver == 2);
    CHECK(res_exit_write(&r, 11, "W02", err, sizeof err) == 0);

    /* write without a lease is rejected; reads need a lease too */
    CHECK(res_execute_write(&r, 11, "W02", WOP_ADD, 1, &v, &ver, err, sizeof err) == -1);
    CHECK(res_read_value(&r, 3, "R04", &v, &ver, err, sizeof err) == -1);

    /* saturating arithmetic keeps the UI's JS numbers exact */
    res_destroy(&r);
    res_init(&r, 10000);
    for (int i = 0; i < 20; i++) {
        res_enter_write(&r, 10, "W01", err, sizeof err);
        res_execute_write(&r, 10, "W01", WOP_MULTIPLY, 1000, &v, &ver, err, sizeof err);
        res_exit_write(&r, 10, "W01", err, sizeof err);
    }
    CHECK(v == JS_SAFE_MAX);
    res_destroy(&r);
}

/* ---------- Strategy behaviour ---------- */
static void test_concurrent_readers_allowed(void) {
    for (int k = 0; k < 3; k++) {
        Strategy s; Flag stop; strat_init(&s, (Algo)k); flag_init(&stop);
        CHECK(strat_reader_enter(&s, &stop, NULL) == 1);
        CHECK(strat_reader_enter(&s, &stop, NULL) == 1);   /* second reader shares the lock */
        SemState st; strat_state(&s, &st);
        CHECK(st.read_count == 2);
        strat_reader_exit(&s); strat_reader_exit(&s);
        strat_state(&s, &st); CHECK(st.read_count == 0);
        flag_destroy(&stop); strat_destroy(&s);
    }
}

static void test_reader_preference_writer_waits(void) {
    Strategy s; Flag stop; strat_init(&s, ALGO_READER_PREF); flag_init(&stop);
    pthread_mutex_t cm = PTHREAD_MUTEX_INITIALIZER; int counter = 0;
    CHECK(strat_reader_enter(&s, &stop, NULL) == 1);

    Actor w; pthread_t tw = start_actor(&w, &s, &stop, 1, &counter, &cm);
    msleep(250);
    CHECK(w.entered == 0);                                  /* writer is blocked behind the reader */
    SemState st; strat_state(&s, &st); CHECK(st.waiting_writers == 1);

    /* a NEW reader overtakes the waiting writer (reader preference) */
    CHECK(strat_reader_enter(&s, &stop, NULL) == 1);
    strat_reader_exit(&s);
    msleep(100); CHECK(w.entered == 0);

    strat_reader_exit(&s);                                  /* last reader leaves */
    pthread_join(tw, NULL);
    CHECK(w.entered == 1);
    strat_writer_exit(&s);
    flag_destroy(&stop); strat_destroy(&s);
}

static void test_writer_preference_blocks_incoming_readers(void) {
    Strategy s; Flag stop; strat_init(&s, ALGO_WRITER_PREF); flag_init(&stop);
    pthread_mutex_t cm = PTHREAD_MUTEX_INITIALIZER; int counter = 0;
    CHECK(strat_reader_enter(&s, &stop, NULL) == 1);        /* R1 reading */

    Actor w, r2;
    pthread_t tw = start_actor(&w, &s, &stop, 1, &counter, &cm);
    msleep(150);
    SemState st; strat_state(&s, &st);
    CHECK(st.read_try_locked == 1);                         /* writer closed the reader gate */
    pthread_t tr = start_actor(&r2, &s, &stop, 0, &counter, &cm);
    msleep(200);
    CHECK(w.entered == 0 && r2.entered == 0);               /* R2 blocked although R1 is only reading */

    strat_reader_exit(&s);                                  /* R1 leaves -> writer goes first */
    pthread_join(tw, NULL);
    CHECK(w.entered == 1);
    msleep(100); CHECK(r2.entered == 0);
    strat_writer_exit(&s);                                  /* writer leaves -> R2 admitted */
    pthread_join(tr, NULL);
    CHECK(r2.entered == 1 && w.order < r2.order);
    strat_reader_exit(&s);
    flag_destroy(&stop); strat_destroy(&s);
}

static void test_fair_fifo_ordering(void) {
    Strategy s; Flag stop; strat_init(&s, ALGO_FAIR_FIFO); flag_init(&stop);
    pthread_mutex_t cm = PTHREAD_MUTEX_INITIALIZER; int counter = 0;
    CHECK(strat_reader_enter(&s, &stop, NULL) == 1);        /* R1 reading */

    Actor w, r2;
    pthread_t tw = start_actor(&w, &s, &stop, 1, &counter, &cm);
    msleep(150);
    pthread_t tr = start_actor(&r2, &s, &stop, 0, &counter, &cm);
    msleep(200);
    CHECK(w.entered == 0 && r2.entered == 0);               /* later reader may NOT bypass the queued writer */

    strat_reader_exit(&s);
    pthread_join(tw, NULL);
    CHECK(w.entered == 1);
    msleep(100); CHECK(r2.entered == 0);
    strat_writer_exit(&s);
    pthread_join(tr, NULL);
    CHECK(r2.entered == 1 && w.order < r2.order);
    strat_reader_exit(&s);
    flag_destroy(&stop); strat_destroy(&s);
}

static void test_cancellation_releases_waiters(void) {
    for (int k = 0; k < 3; k++) {
        Strategy s; Flag stop; strat_init(&s, (Algo)k); flag_init(&stop);
        pthread_mutex_t cm = PTHREAD_MUTEX_INITIALIZER; int counter = 0;
        CHECK(strat_writer_enter(&s, &stop, NULL) == 1);
        Actor w, r;
        pthread_t tw = start_actor(&w, &s, &stop, 1, &counter, &cm);
        pthread_t tr = start_actor(&r, &s, &stop, 0, &counter, &cm);
        msleep(150);
        flag_set(&stop);
        pthread_join(tw, NULL); pthread_join(tr, NULL);
        CHECK(w.entered == 0 && r.entered == 0);
        SemState st; strat_state(&s, &st);
        CHECK(st.waiting_readers == 0 && st.waiting_writers == 0);
        flag_destroy(&stop); strat_destroy(&s);
    }
}

/* ---------- Engine ---------- */
static SimConfig quick_cfg(Algo a, int readers, int writers, int rd_ms, int wr_ms, int stagger) {
    SimConfig c;
    simconfig_default(&c);
    c.algorithm = a; c.reader_count = readers; c.writer_count = writers;
    c.read_duration_ms = rd_ms; c.write_duration_ms = wr_ms; c.stagger_arrivals_ms = stagger;
    c.initial_value = 100; c.writer_op = WOP_ADD; c.writer_amount = 10; c.max_duration_sec = 60;
    return c;
}

static void wait_done(Engine *e, int max_ms) {
    for (int i = 0; i < max_ms / 20; i++) {
        SimStatus s = engine_status(e);
        if (s != SS_RUNNING && s != SS_PAUSED) break;
        msleep(20);
    }
    engine_join(e);
}

static int count_events(Engine *e, const char *etype) {
    StrBuf sb; sb_init(&sb);
    engine_events_json(e, &sb, 0, etype, -1, 100000);
    int n = 0;
    for (char *p = sb.data; (p = strstr(p, "\"seq\":")); p += 6) n++;
    sb_free(&sb);
    return n;
}

static void test_simulation_engine_full_run(void) {
    for (int k = 0; k < 3; k++) {
        SimConfig c = quick_cfg((Algo)k, 6, 3, 100, 120, 15);
        Engine *e = engine_create(&c, NULL, NULL);
        engine_start(e);
        wait_done(e, 20000);
        CHECK(engine_status(e) == SS_COMPLETED);
        int64_t v; int ver, tot;
        engine_summary(e, &v, &ver, &tot);
        CHECK(v == 130 && ver == 3);
        Metrics m; engine_metrics(e, &m);
        CHECK(m.completed_reads == 6 && m.completed_writes == 3);
        CHECK(count_events(e, "INVARIANT_VIOLATION") == 0);
        CHECK(tot == 6 * 3 + 3 * 3 + 2);   /* 3 events per process + STARTED + COMPLETED */
        for (int i = 0; i < e->nprocs; i++) CHECK(e->procs[i].state == PS_COMPLETED);
        engine_destroy(e);
    }
}

static void test_simulation_clean_cancellation(void) {
    for (int k = 0; k < 3; k++) {
        SimConfig c = quick_cfg((Algo)k, 20, 8, 3000, 3000, 20);
        Engine *e = engine_create(&c, NULL, NULL);
        engine_start(e);
        msleep(400);
        double t0 = mono_now();
        engine_stop(e);
        engine_join(e);
        CHECK(mono_now() - t0 < 1.5);                       /* all 28 threads wound down promptly */
        CHECK(engine_status(e) == SS_CANCELLED);
        for (int i = 0; i < e->nprocs; i++)
            CHECK(e->procs[i].state == PS_COMPLETED || e->procs[i].state == PS_CANCELLED);
        int nr, wi; res_snapshot(&e->res, NULL, NULL, &nr, &wi);
        CHECK(nr == 0 && wi == -1);                         /* no leaked leases */
        engine_destroy(e);
    }
}

static void test_simulation_pause_resume(void) {
    SimConfig c = quick_cfg(ALGO_FAIR_FIFO, 3, 1, 400, 400, 0);
    Engine *e = engine_create(&c, NULL, NULL);
    engine_start(e);
    msleep(150);
    engine_pause(e);
    CHECK(engine_status(e) == SS_PAUSED);
    double before[MAX_PROCS];
    pthread_mutex_lock(&e->lock);
    for (int i = 0; i < e->nprocs; i++) before[i] = e->procs[i].progress_pct;
    pthread_mutex_unlock(&e->lock);
    msleep(500);
    pthread_mutex_lock(&e->lock);
    for (int i = 0; i < e->nprocs; i++) CHECK(before[i] == e->procs[i].progress_pct);
    pthread_mutex_unlock(&e->lock);
    Metrics m1, m2; engine_metrics(e, &m1); msleep(200); engine_metrics(e, &m2);
    CHECK(m2.total_elapsed_ms - m1.total_elapsed_ms < 5.0);   /* paused time is not counted */
    engine_resume(e);
    wait_done(e, 20000);
    CHECK(engine_status(e) == SS_COMPLETED);
    engine_destroy(e);
}

static void test_reset_and_rerun(void) {
    SimConfig c = quick_cfg(ALGO_READER_PREF, 3, 2, 60, 60, 5);
    Engine *e = engine_create(&c, NULL, NULL);
    for (int round = 0; round < 3; round++) {
        engine_start(e);
        wait_done(e, 10000);
        int64_t v; int ver, tot;
        engine_summary(e, &v, &ver, &tot);
        CHECK(engine_status(e) == SS_COMPLETED && v == 120 && ver == 2);
        engine_reset(e);
        engine_summary(e, &v, &ver, &tot);
        CHECK(engine_status(e) == SS_IDLE && v == 100 && ver == 0 && tot == 1);
    }
    engine_destroy(e);
}

static void test_timeout_stops_run(void) {
    SimConfig c = quick_cfg(ALGO_READER_PREF, 2, 1, 10000, 10000, 0);
    c.max_duration_sec = 1;
    Engine *e = engine_create(&c, NULL, NULL);
    engine_start(e);
    msleep(1700);
    CHECK(engine_status(e) == SS_CANCELLED);
    CHECK(count_events(e, "TIMEOUT") == 1);
    engine_destroy(e);
}

/* 100 readers + 50 writers hammering each algorithm: no violations, no deadlock, exact final value */
static void test_max_scale_stress(void) {
    for (int k = 0; k < 3; k++) {
        SimConfig c = quick_cfg((Algo)k, 100, 50, 30, 30, 0);
        Engine *e = engine_create(&c, NULL, NULL);
        engine_start(e);
        wait_done(e, 40000);
        CHECK(engine_status(e) == SS_COMPLETED);
        int64_t v; int ver, tot;
        engine_summary(e, &v, &ver, &tot);
        CHECK(v == 100 + 50 * 10 && ver == 50);
        CHECK(count_events(e, "INVARIANT_VIOLATION") == 0);
        engine_destroy(e);
    }
}

int main(void) {
    RUN(test_websocket_accept_vector);
    RUN(test_json_object_reader);
    RUN(test_python_style_float_format);
    RUN(test_shared_resource_invariants);
    RUN(test_concurrent_readers_allowed);
    RUN(test_reader_preference_writer_waits);
    RUN(test_writer_preference_blocks_incoming_readers);
    RUN(test_fair_fifo_ordering);
    RUN(test_cancellation_releases_waiters);
    RUN(test_simulation_engine_full_run);
    RUN(test_simulation_clean_cancellation);
    RUN(test_simulation_pause_resume);
    RUN(test_reset_and_rerun);
    RUN(test_timeout_stops_run);
    RUN(test_max_scale_stress);
    printf("\n%d tests, %d failed\n", g_run, g_failed);
    return g_failed ? 1 : 0;
}
