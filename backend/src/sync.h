/*
 * sync.h - Reader-Writer synchronization core for SyncLab.
 *
 *   Flag      : cancellable event (stop / pause)
 *   Resource  : protected shared register with invariant checks + occupancy accounting
 *   Strategy  : the three semaphore algorithms (reader pref, writer pref, fair FIFO)
 *   Engine    : thread lifecycle, event log, metrics, JSON serialisation
 *
 * Everything is built on POSIX threads (pthread) and POSIX semaphores (sem_t).
 */
#ifndef SYNCLAB_SYNC_H
#define SYNCLAB_SYNC_H

#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdint.h>

#include "util.h"

#define MAX_READERS 100
#define MAX_WRITERS 50
#define MAX_PROCS   (MAX_READERS + MAX_WRITERS)
#define JS_SAFE_MAX 9007199254740991LL /* resource value saturates here so the UI stays exact */

typedef enum { ALGO_READER_PREF = 0, ALGO_WRITER_PREF, ALGO_FAIR_FIFO } Algo;
typedef enum { WOP_ADD = 0, WOP_SUBTRACT, WOP_MULTIPLY, WOP_SET } WOp;
typedef enum { PT_READER = 0, PT_WRITER } PType;
typedef enum { PS_CREATED = 0, PS_WAITING, PS_READING, PS_WRITING, PS_COMPLETED, PS_CANCELLED, PS_ERROR } PState;
typedef enum { SS_IDLE = 0, SS_RUNNING, SS_PAUSED, SS_COMPLETED, SS_CANCELLED, SS_ERROR } SimStatus;

const char *algo_name(Algo a);
const char *wop_name(WOp o);
const char *ptype_name(PType t);
const char *pstate_name(PState s);
const char *status_name(SimStatus s);
int algo_parse(const char *s, Algo *out);
int wop_parse(const char *s, WOp *out);

/* ---------------- Flag (event) ---------------- */
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t  c;
    int             flag;
} Flag;

void flag_init(Flag *f);
void flag_destroy(Flag *f);
void flag_set(Flag *f);
void flag_clear(Flag *f);
int  flag_is_set(Flag *f);
int  flag_wait(Flag *f, double timeout_sec); /* returns flag state after waiting */

/* ---------------- Resource ---------------- */
typedef struct {
    pthread_mutex_t m;
    int64_t value;
    int     version;
    char    active[MAX_PROCS];
    int     n_active_readers;
    int     active_writer;          /* process index or -1 */
    char    active_writer_id[16];
    double  occupied_time;
    double  last_change;
    int     is_occupied;
} Resource;

void res_init(Resource *r, int64_t initial);
void res_destroy(Resource *r);
/* all return 0 on success, -1 on an invariant violation (message in err) */
int  res_enter_read(Resource *r, int idx, const char *pid, char *err, size_t en);
int  res_read_value(Resource *r, int idx, const char *pid, int64_t *val, int *ver, char *err, size_t en);
int  res_exit_read(Resource *r, int idx, const char *pid, char *err, size_t en);
int  res_enter_write(Resource *r, int idx, const char *pid, char *err, size_t en);
int  res_execute_write(Resource *r, int idx, const char *pid, WOp op, int amount, int64_t *val, int *ver, char *err, size_t en);
int  res_exit_write(Resource *r, int idx, const char *pid, char *err, size_t en);
void res_snapshot(Resource *r, int64_t *val, int *ver, int *n_readers, int *writer_idx);
double res_occupancy_pct(Resource *r, double total_elapsed_sec);

/* ---------------- Strategies ---------------- */
typedef struct {
    int read_count, write_count, waiting_readers, waiting_writers;
    int resource_locked, read_try_locked, turnstile_busy;
    const char *admission_policy;
} SemState;

typedef struct {
    Algo kind;
    pthread_mutex_t state_lock;   /* guards the waiting_* counters */
    int waiting_readers, waiting_writers;
    sem_t mutex;                  /* protects read_count (mutex_r)            */
    sem_t mutex_w;                /* protects write_count (writer preference) */
    sem_t read_try;               /* reader gate (writer preference)          */
    sem_t resource;               /* the "wrt" semaphore                      */
    sem_t order;                  /* FIFO turnstile (fair)                    */
    atomic_int read_count;
    atomic_int write_count;
} Strategy;

void strat_init(Strategy *s, Algo kind);
void strat_destroy(Strategy *s);
/* enter returns 1 if admitted, 0 if cancelled via stop. pause may be NULL. */
int  strat_reader_enter(Strategy *s, Flag *stop, Flag *pause);
void strat_reader_exit(Strategy *s);
int  strat_writer_enter(Strategy *s, Flag *stop, Flag *pause);
void strat_writer_exit(Strategy *s);
void strat_state(Strategy *s, SemState *out);

/* ---------------- Engine ---------------- */
typedef struct {
    Algo algorithm;
    int  reader_count, writer_count;
    int  read_duration_ms, write_duration_ms;
    int  initial_value;
    WOp  writer_op;
    int  writer_amount;
    int  max_duration_sec;
    int  stagger_arrivals_ms;
} SimConfig;

void simconfig_default(SimConfig *c);
void simconfig_json(StrBuf *sb, const SimConfig *c);

typedef struct {
    char    id[16];
    PType   type;
    PState  state;
    double  created_at;
    int     has_requested; double requested_at;  /* monotonic seconds (as the UI expects) */
    int     has_admitted;  double admitted_at;
    int     has_completed; double completed_at;  /* epoch seconds */
    double  wait_time_ms, duration_ms, progress_pct;
    int     has_op; WOp op;
    int     has_amount; int amount;
    int     has_error; char error[160];
} Proc;

typedef struct {
    int     seq;
    double  rel_ms;
    char    iso[40];
    char    pid[16];
    int     has_pid;
    int     ptype;     /* -1 none */
    char    etype[40];
    int     from, to;  /* -1 none */
    int64_t value;
    int     version;
    int     active_readers, active_writers, waiting_readers, waiting_writers;
    char    details[320];
} SimEvent;

typedef struct {
    int total_readers, total_writers, completed_reads, completed_writes;
    double avg_reader_wait_ms, avg_writer_wait_ms, max_reader_wait_ms, max_writer_wait_ms;
    double total_elapsed_ms, throughput_ops_per_sec, resource_occupancy_pct;
    int starvation_warning;
    char starvation_details[200];
} Metrics;

/* Called (from worker threads) with each event serialised as one JSON object. */
typedef void (*EventCb)(const char *json, size_t len, void *ctx);

typedef struct Engine Engine;
struct Engine {
    char      simulation_id[16];
    SimConfig cfg;
    EventCb   cb;
    void     *cb_ctx;
    int       broadcast;     /* 0 silences the callback */

    pthread_mutex_t lock;
    SimStatus status;
    Flag stop, pause;        /* pause flag SET  = running, CLEAR = paused */
    double pause_start_mono, total_paused_sec, start_mono, end_mono;

    Strategy strat;
    Resource res;

    Proc procs[MAX_PROCS];
    int  nprocs;

    pthread_t threads[MAX_PROCS];
    int       nthreads;
    pthread_t supervisor;
    int       has_supervisor;

    int        seq;
    SimEvent  *events;
    int        nev, capev;
};

Engine *engine_create(const SimConfig *cfg, EventCb cb, void *ctx);
void    engine_destroy(Engine *e);           /* stop + join + free */
void    engine_start(Engine *e);
void    engine_pause(Engine *e);
void    engine_resume(Engine *e);
void    engine_stop(Engine *e);
void    engine_reset(Engine *e);
void    engine_join(Engine *e);              /* wait for all worker + supervisor threads */
SimStatus engine_status(Engine *e);

void engine_metrics(Engine *e, Metrics *out);
void engine_metrics_json(StrBuf *sb, const Metrics *m);
void engine_summary(Engine *e, int64_t *value, int *version, int *total_events);
void engine_state_json(Engine *e, StrBuf *sb);
void engine_results_json(Engine *e, StrBuf *sb);
void engine_event_json(StrBuf *sb, const SimEvent *ev);
/* filters: min_seq (exclusive), etype (NULL = any), ptype (-1 = any), limit */
void engine_events_json(Engine *e, StrBuf *sb, int min_seq, const char *etype, int ptype, int limit);
void engine_events_csv(Engine *e, StrBuf *sb);
/* Events with seq > last_seq, one JSON object per line (no raw newlines occur inside a JSON string). */
void engine_events_after(Engine *e, int last_seq, StrBuf *sb);

#endif
