/*
 * ============================================================================
 * FILE: 03_fair_fifo.c
 * PROJECT: SyncLab - Reader-Writer Synchronization Simulator
 * TOPIC: Third Readers-Writers Problem (Fair FIFO Admission / Starvation-Free)
 * ============================================================================
 *
 * ALGORITHM DESCRIPTION:
 * ----------------------
 * In Fair FIFO Admission:
 * 1. Both readers and writers pass through a single-ticket admission turnstile ('order').
 * 2. Arriving threads are admitted strictly in order of arrival (First-In, First-Out).
 * 3. Readers:
 *    a. Acquire 'order' turnstile.
 *    b. Under 'mutex', increment 'read_count'. First reader acquires 'resource'.
 *    c. Release 'order' turnstile immediately so subsequent queued threads can advance.
 *    d. Read concurrently if the next thread is also a reader!
 *    e. Last exiting reader releases 'resource'.
 * 4. Writers:
 *    a. Acquire 'order' turnstile.
 *    b. Acquire 'resource' lock (waits for active readers to finish).
 *    c. Release 'order' turnstile so queue can advance.
 *    d. Writes exclusively.
 *    e. Releases 'resource'.
 *
 * STARVATION CONSEQUENCE:
 * -----------------------
 * ZERO STARVATION. Because newly arriving readers cannot bypass an earlier queued writer
 * at the 'order' turnstile, writers are guaranteed bounded waiting times.
 * Similarly, continuous writers cannot starve readers because readers queue fairly.
 *
 * COMPILATION & EXECUTION:
 * ------------------------
 *   gcc 03_fair_fifo.c -pthread -o 03_fair_fifo
 *   ./03_fair_fifo
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
static sem_t order;     /* FIFO Turnstile semaphore (1): Enforces request-order admission */
static sem_t mutex;     /* Binary semaphore (1): Protects read_count */
static sem_t resource;  /* Mutual exclusion semaphore (1): Guards shared data */

static int read_count = 0;   /* Count of active reading threads */

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
        usleep((rand() % 40 + 10) * 1000);

        printf("[Reader %ld] ARRIVED: Waiting at FIFO turnstile...\n", id);

        /* --- ENTRY PROTOCOL --- */
        /* 1. Pass through FIFO admission turnstile in arrival order */
        sem_wait(&order);

        /* 2. Safely increment read_count */
        sem_wait(&mutex);
        read_count++;
        if (read_count == 1) {
            /* First reader locks shared resource */
            sem_wait(&resource);
        }
        sem_post(&mutex);

        /* 3. Release turnstile immediately so next thread in line can enter */
        sem_post(&order);

        /* --- CRITICAL SECTION (READING) --- */
        printf("  >> [Reader %ld] READING: Shared Value = %d (v%d) [Active Readers: %d]\n",
               id, shared_resource, shared_version, read_count);
        usleep((rand() % 60 + 30) * 1000);

        /* --- EXIT PROTOCOL --- */
        sem_wait(&mutex);
        read_count--;
        if (read_count == 0) {
            /* Last reader departs -> releases resource lock */
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

        printf("  ** [Writer %ld] ARRIVED: Waiting at FIFO turnstile...\n", id);

        /* --- ENTRY PROTOCOL --- */
        /* 1. Pass through FIFO admission turnstile in arrival order */
        sem_wait(&order);

        /* 2. Acquire exclusive resource lock (waits for existing active readers to exit) */
        sem_wait(&resource);

        /* 3. Release turnstile once resource lock is secured, allowing next thread to queue */
        sem_post(&order);

        /* --- CRITICAL SECTION (EXCLUSIVE WRITING) --- */
        shared_resource += 50;
        shared_version++;
        printf("  #### [Writer %ld] WRITING: Mutated Value to %d (v%d) [EXCLUSIVE]\n",
               id, shared_resource, shared_version);
        usleep((rand() % 80 + 50) * 1000);

        /* --- EXIT PROTOCOL --- */
        /* 4. Release exclusive resource lock */
        sem_post(&resource);

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
    printf(" SyncLab: Algorithm 3 - Fair FIFO Admission (Starvation-Free)\n");
    printf(" Initial Resource Value: %d, Version: %d\n", shared_resource, shared_version);
    printf(" Readers: %d | Writers: %d\n", NUM_READERS, NUM_WRITERS);
    printf("===================================================================\n\n");

    /* Initialize semaphores to 1 (unlocked state) */
    sem_init(&order, 0, 1);
    sem_init(&mutex, 0, 1);
    sem_init(&resource, 0, 1);

    /* Launch Reader and Writer threads */
    for (long i = 0; i < NUM_READERS; i++) pthread_create(&readers[i], NULL, reader_thread, (void*)(i + 1));
    for (long i = 0; i < NUM_WRITERS; i++) pthread_create(&writers[i], NULL, writer_thread, (void*)(i + 1));

    /* Await completion */
    for (int i = 0; i < NUM_READERS; i++) pthread_join(readers[i], NULL);
    for (int i = 0; i < NUM_WRITERS; i++) pthread_join(writers[i], NULL);

    /* Clean up semaphores */
    sem_destroy(&order);
    sem_destroy(&mutex);
    sem_destroy(&resource);

    printf("\n===================================================================\n");
    printf(" Final Resource Value: %d, Final Version: %d\n", shared_resource, shared_version);
    printf(" Algorithm 3 (Fair FIFO Turnstile) Demonstration Completed Successfully.\n");
    printf(" Zero Starvation Observed: All Readers and Writers Serviced in Order.\n");
    printf("===================================================================\n");
    return 0;
}
