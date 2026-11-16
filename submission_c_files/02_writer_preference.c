/*
 * ============================================================================
 * FILE: 02_writer_preference.c
 * PROJECT: SyncLab - Reader-Writer Synchronization Simulator
 * TOPIC: Second Readers-Writers Problem (Writer Preference / Courtois et al. 1971)
 * ============================================================================
 *
 * ALGORITHM DESCRIPTION:
 * ----------------------
 * In Writer Preference:
 * 1. Writers are given strict priority over newly arriving readers.
 * 2. When a writer signals its intent to write:
 *    a. It acquires 'mutex_w' and increments 'write_count'.
 *    b. The FIRST queued writer (write_count == 1) acquires the 'read_try' gate.
 *       This gate immediately blocks any newly arriving readers from entering.
 *    c. The writer then waits on 'resource' until currently active readers exit.
 * 3. Arriving readers must pass through 'read_try' before modifying 'read_count'.
 *    Since 'read_try' is locked by waiting writers, new readers queue outside.
 * 4. When all writers finish (write_count == 0), the LAST writer releases 'read_try',
 *    allowing waiting readers to proceed.
 *
 * STARVATION CONSEQUENCE:
 * -----------------------
 * If writers arrive continuously, 'write_count' never drops to 0.
 * 'read_try' remains locked, causing indefinite READER STARVATION.
 *
 * COMPILATION & EXECUTION:
 * ------------------------
 *   gcc 02_writer_preference.c -pthread -o 02_writer_pref
 *   ./02_writer_pref
 * ============================================================================
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>

#define NUM_READERS 5
#define NUM_WRITERS 3
#define READ_ITERATIONS 2
#define WRITE_ITERATIONS 2

/* ------------------- Synchronization Primitives ------------------- */
static sem_t mutex;     /* Binary semaphore (1): Protects read_count */
static sem_t mutex_w;   /* Binary semaphore (1): Protects write_count */
static sem_t read_try;  /* Gate semaphore (1): Held by writers to block incoming readers */
static sem_t resource;  /* Mutual exclusion semaphore (1): Guards shared data */

static int read_count = 0;   /* Count of active reading threads */
static int write_count = 0;  /* Count of waiting or active writing threads */

/* ------------------- Shared Data / Resource ----------------------- */
static int shared_resource = 100;
static int shared_version = 0;

/*
 * ============================================================================
 * READER THREAD LOGIC
 * ============================================================================
 */
void* reader_thread(void* arg) {
    long id = (long)arg;

    for (int i = 0; i < READ_ITERATIONS; i++) {
        usleep((rand() % 50 + 10) * 1000);

        printf("[Reader %ld] REQUESTING entry (checking read_try gate)...\n", id);

        /* --- ENTRY PROTOCOL --- */
        /* 1. Arriving reader must pass the writer-controlled admission gate */
        sem_wait(&read_try);

        /* 2. Increment read_count safely under 'mutex' */
        sem_wait(&mutex);
        read_count++;
        if (read_count == 1) {
            /* First reader acquires shared resource lock against idle writers */
            sem_wait(&resource);
        }
        sem_post(&mutex);

        /* 3. Re-open gate so fellow admitted readers can pass through */
        sem_post(&read_try);

        /* --- CRITICAL SECTION (READING) --- */
        printf("  >> [Reader %ld] READING: Shared Value = %d (v%d) [Active Readers: %d]\n",
               id, shared_resource, shared_version, read_count);
        usleep((rand() % 60 + 30) * 1000);

        /* --- EXIT PROTOCOL --- */
        sem_wait(&mutex);
        read_count--;
        if (read_count == 0) {
            /* Last reader departs -> releases resource lock for waiting writers */
            printf("[Reader %ld] Last reader departing -> Releasing resource.\n", id);
            sem_post(&resource);
        }
        sem_post(&mutex);

        printf("[Reader %ld] COMPLETED read %d/%d.\n", id, i + 1, READ_ITERATIONS);
    }
    return NULL;
}

/*
 * ============================================================================
 * WRITER THREAD LOGIC
 * ============================================================================
 */
void* writer_thread(void* arg) {
    long id = (long)arg;

    for (int i = 0; i < WRITE_ITERATIONS; i++) {
        usleep((rand() % 40 + 20) * 1000);

        printf("  ** [Writer %ld] ARRIVED: Registering in write queue...\n", id);

        /* --- ENTRY PROTOCOL --- */
        /* 1. Safely update write_count under 'mutex_w' */
        sem_wait(&mutex_w);
        write_count++;
        if (write_count == 1) {
            /* First arriving writer immediately locks 'read_try' gate */
            printf("  ** [Writer %ld] FIRST WRITER -> Locking read_try gate against new readers!\n", id);
            sem_wait(&read_try);
        }
        sem_post(&mutex_w);

        /* 2. Acquire exclusive resource lock (waits for active readers to finish) */
        sem_wait(&resource);

        /* --- CRITICAL SECTION (EXCLUSIVE WRITING) --- */
        shared_resource += 25;
        shared_version++;
        printf("  #### [Writer %ld] WRITING: Mutated Value to %d (v%d) [EXCLUSIVE]\n",
               id, shared_resource, shared_version);
        usleep((rand() % 80 + 50) * 1000);

        /* --- EXIT PROTOCOL --- */
        /* 3. Release exclusive resource lock */
        sem_post(&resource);

        /* 4. Safely decrement write_count under 'mutex_w' */
        sem_wait(&mutex_w);
        write_count--;
        if (write_count == 0) {
            /* Last queued writer re-opens 'read_try' gate for readers */
            printf("  ** [Writer %ld] LAST WRITER -> Re-opening read_try gate for readers.\n", id);
            sem_post(&read_try);
        }
        sem_post(&mutex_w);

        printf("  ** [Writer %ld] COMPLETED write %d/%d.\n", id, i + 1, WRITE_ITERATIONS);
    }
    return NULL;
}

/*
 * ============================================================================
 * MAIN ENTRY POINT
 * ============================================================================
 */
int main(void) {
    pthread_t readers[NUM_READERS];
    pthread_t writers[NUM_WRITERS];

    printf("===================================================================\n");
    printf(" SyncLab: Algorithm 2 - Writer Preference (Courtois et al. 1971)\n");
    printf(" Initial Resource Value: %d, Version: %d\n", shared_resource, shared_version);
    printf(" Readers: %d | Writers: %d\n", NUM_READERS, NUM_WRITERS);
    printf("===================================================================\n\n");

    /* Initialize semaphores to 1 (unlocked state) */
    sem_init(&mutex, 0, 1);
    sem_init(&mutex_w, 0, 1);
    sem_init(&read_try, 0, 1);
    sem_init(&resource, 0, 1);

    /* Launch Reader and Writer threads */
    for (long i = 0; i < NUM_READERS; i++) pthread_create(&readers[i], NULL, reader_thread, (void*)(i + 1));
    for (long i = 0; i < NUM_WRITERS; i++) pthread_create(&writers[i], NULL, writer_thread, (void*)(i + 1));

    /* Await completion */
    for (int i = 0; i < NUM_READERS; i++) pthread_join(readers[i], NULL);
    for (int i = 0; i < NUM_WRITERS; i++) pthread_join(writers[i], NULL);

    /* Clean up semaphores */
    sem_destroy(&mutex);
    sem_destroy(&mutex_w);
    sem_destroy(&read_try);
    sem_destroy(&resource);

    printf("\n===================================================================\n");
    printf(" Final Resource Value: %d, Final Version: %d\n", shared_resource, shared_version);
    printf(" Algorithm 2 (Writer Preference) Demonstration Completed Successfully.\n");
    printf("===================================================================\n");
    return 0;
}
