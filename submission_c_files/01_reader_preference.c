/*
 * ============================================================================
 * FILE: 01_reader_preference.c
 * PROJECT: SyncLab - Reader-Writer Synchronization Simulator
 * TOPIC: First Readers-Writers Problem (Reader Preference / Courtois et al. 1971)
 * ============================================================================
 *
 * ALGORITHM DESCRIPTION:
 * ----------------------
 * In Reader Preference:
 * 1. Any reader can enter the critical section if another reader is already inside.
 * 2. The FIRST arriving reader acquires the mutual exclusion lock on the shared
 *    resource ('wrt' / 'resource').
 * 3. Subsequent readers bypass the resource lock and increment 'read_count'.
 * 4. All active readers read the shared memory concurrently.
 * 5. The LAST departing reader (when read_count reaches 0) releases 'resource'.
 * 6. Writers must wait until ALL active readers have departed.
 *
 * STARVATION CONSEQUENCE:
 * -----------------------
 * If a continuous stream of readers arrives, 'read_count' never drops to 0.
 * As a result, waiting writers experience INDEFINITE STARVATION.
 *
 * COMPILATION & EXECUTION:
 * ------------------------
 *   gcc 01_reader_preference.c -pthread -o 01_reader_pref
 *   ./01_reader_pref
 * ============================================================================
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>

#define NUM_READERS 5
#define NUM_WRITERS 2
#define READ_ITERATIONS 3
#define WRITE_ITERATIONS 2

/* ------------------- Synchronization Primitives ------------------- */
static sem_t mutex;     /* Binary semaphore (1): Protects access to read_count */
static sem_t resource;  /* Binary semaphore (1): Mutual exclusion lock on shared data */
static int read_count = 0;   /* Number of readers currently reading or waiting inside */

/* ------------------- Shared Data / Resource ----------------------- */
static int shared_resource = 100; /* Protected register value */
static int shared_version = 0;    /* Monotonically increasing version counter */

/*
 * ============================================================================
 * READER THREAD LOGIC
 * ============================================================================
 */
void* reader_thread(void* arg) {
    long id = (long)arg;

    for (int i = 0; i < READ_ITERATIONS; i++) {
        usleep((rand() % 40 + 10) * 1000); /* Stagger arrival slightly */

        printf("[Reader %ld] REQUESTING entry...\n", id);

        /* --- ENTRY PROTOCOL --- */
        sem_wait(&mutex); /* Lock mutex to safely modify read_count */
        read_count++;
        if (read_count == 1) {
            /* First reader must lock the shared resource against writers */
            printf("[Reader %ld] First reader -> Locking resource against writers.\n", id);
            sem_wait(&resource);
        }
        sem_post(&mutex); /* Release mutex so other readers can enter */

        /* --- CRITICAL SECTION (READING) --- */
        printf("  >> [Reader %ld] READING: Shared Value = %d (v%d) [Active Readers: %d]\n",
               id, shared_resource, shared_version, read_count);
        usleep((rand() % 60 + 40) * 1000); /* Simulate read duration */

        /* --- EXIT PROTOCOL --- */
        sem_wait(&mutex); /* Lock mutex to safely modify read_count */
        read_count--;
        if (read_count == 0) {
            /* Last reader must unlock the resource so waiting writers can proceed */
            printf("[Reader %ld] Last reader departing -> Unlocking resource for writers.\n", id);
            sem_post(&resource);
        }
        sem_post(&mutex); /* Release mutex */

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
        usleep((rand() % 60 + 30) * 1000); /* Stagger arrival */

        printf("  ** [Writer %ld] REQUESTING exclusive write access...\n", id);

        /* --- ENTRY PROTOCOL --- */
        /* Writer must acquire exclusive lock on shared resource */
        sem_wait(&resource);

        /* --- CRITICAL SECTION (EXCLUSIVE WRITING) --- */
        shared_resource += 10;
        shared_version++;
        printf("  #### [Writer %ld] WRITING: Mutated Value to %d (v%d) [EXCLUSIVE]\n",
               id, shared_resource, shared_version);
        usleep((rand() % 80 + 50) * 1000); /* Simulate write duration */

        /* --- EXIT PROTOCOL --- */
        sem_post(&resource); /* Release exclusive lock */

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
    printf(" SyncLab: Algorithm 1 - Reader Preference (Courtois et al. 1971)\n");
    printf(" Initial Resource Value: %d, Version: %d\n", shared_resource, shared_version);
    printf(" Readers: %d | Writers: %d\n", NUM_READERS, NUM_WRITERS);
    printf("===================================================================\n\n");

    /* Initialize semaphores to 1 (unlocked binary state) */
    sem_init(&mutex, 0, 1);
    sem_init(&resource, 0, 1);

    /* Launch Reader and Writer threads */
    for (long i = 0; i < NUM_READERS; i++) pthread_create(&readers[i], NULL, reader_thread, (void*)(i + 1));
    for (long i = 0; i < NUM_WRITERS; i++) pthread_create(&writers[i], NULL, writer_thread, (void*)(i + 1));

    /* Await completion of all threads */
    for (int i = 0; i < NUM_READERS; i++) pthread_join(readers[i], NULL);
    for (int i = 0; i < NUM_WRITERS; i++) pthread_join(writers[i], NULL);

    /* Clean up semaphores */
    sem_destroy(&mutex);
    sem_destroy(&resource);

    printf("\n===================================================================\n");
    printf(" Final Resource Value: %d, Final Version: %d\n", shared_resource, shared_version);
    printf(" Algorithm 1 (Reader Preference) Demonstration Completed Successfully.\n");
    printf("===================================================================\n");
    return 0;
}
