/**
 * Anamnesis Queue Tests
 */

#include "anamnesis_queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

#define TEST_ASSERT(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); exit(1); } } while(0)
#define TEST_BEGIN(name) printf("[Test] %s...", name); fflush(stdout)
#define TEST_PASS() printf(" PASSED\n")

void test_queue_basic(void) {
    TEST_BEGIN("queue_basic");
    
    AnamQueueConfig cfg = { .item_size = sizeof(int), .capacity = 100 };
    AnamQueue* q = anam_queue_create(&cfg);
    TEST_ASSERT(q != NULL, "queue creation failed");
    TEST_ASSERT(anam_queue_empty(q), "new queue should be empty");
    
    for (int i = 0; i < 100; i++) {
        AnamHandle h = anam_queue_push(q, &i);
        TEST_ASSERT(!anam_is_null(h), "push failed");
    }
    
    for (int i = 0; i < 100; i++) {
        int out;
        TEST_ASSERT(anam_queue_pop(q, &out), "pop failed");
        TEST_ASSERT(out == i, "FIFO order violated");
    }
    
    TEST_ASSERT(anam_queue_empty(q), "queue should be empty");
    
    anam_queue_destroy(q);
    TEST_PASS();
}

#define NUM_THREADS 4
#define ITEMS_PER_THREAD 5000

typedef struct {
    AnamQueue* queue;
    int id;
    atomic_int* produced;
    atomic_int* consumed;
    atomic_int* sum;
} ThreadCtx;

#ifdef _WIN32
DWORD WINAPI producer(LPVOID arg) {
#else
void* producer(void* arg) {
#endif
    ThreadCtx* ctx = (ThreadCtx*)arg;
    for (int i = 0; i < ITEMS_PER_THREAD; i++) {
        int val = ctx->id * ITEMS_PER_THREAD + i;
        while (anam_is_null(anam_queue_push(ctx->queue, &val))) {
#ifdef _WIN32
            Sleep(0);
#else
            usleep(1);
#endif
        }
        atomic_fetch_add(ctx->produced, 1);
    }
    return 0;
}

#ifdef _WIN32
DWORD WINAPI consumer(LPVOID arg) {
#else
void* consumer(void* arg) {
#endif
    ThreadCtx* ctx = (ThreadCtx*)arg;
    int total = NUM_THREADS * ITEMS_PER_THREAD;
    while (atomic_load(ctx->consumed) < total) {
        int val;
        if (anam_queue_pop(ctx->queue, &val)) {
            atomic_fetch_add(ctx->consumed, 1);
            atomic_fetch_add(ctx->sum, val);
        } else if (atomic_load(ctx->produced) >= total) {
            while (anam_queue_pop(ctx->queue, &val)) {
                atomic_fetch_add(ctx->consumed, 1);
                atomic_fetch_add(ctx->sum, val);
            }
            break;
        }
    }
    return 0;
}

void test_queue_concurrent(void) {
    TEST_BEGIN("queue_concurrent");

    AnamQueueConfig cfg = { .item_size = sizeof(int), .capacity = 5000 };
    AnamQueue* q = anam_queue_create(&cfg);
    
    atomic_int produced = 0, consumed = 0, sum = 0;
    ThreadCtx pctx[NUM_THREADS], cctx[NUM_THREADS];
    
    for (int i = 0; i < NUM_THREADS; i++) {
        pctx[i] = (ThreadCtx){ q, i, &produced, &consumed, &sum };
        cctx[i] = (ThreadCtx){ q, i, &produced, &consumed, &sum };
    }
    
#ifdef _WIN32
    HANDLE threads[NUM_THREADS * 2];
    for (int i = 0; i < NUM_THREADS; i++) {
        threads[i] = CreateThread(NULL, 0, producer, &pctx[i], 0, NULL);
        threads[i + NUM_THREADS] = CreateThread(NULL, 0, consumer, &cctx[i], 0, NULL);
    }
    WaitForMultipleObjects(NUM_THREADS * 2, threads, TRUE, INFINITE);
    for (int i = 0; i < NUM_THREADS * 2; i++) CloseHandle(threads[i]);
#else
    pthread_t threads[NUM_THREADS * 2];
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_create(&threads[i], NULL, producer, &pctx[i]);
        pthread_create(&threads[i + NUM_THREADS], NULL, consumer, &cctx[i]);
    }
    for (int i = 0; i < NUM_THREADS * 2; i++) pthread_join(threads[i], NULL);
#endif
    
    int total = NUM_THREADS * ITEMS_PER_THREAD;
    int expected = 0;
    for (int i = 0; i < total; i++) expected += i;
    
    printf(" (sum=%d, expected=%d)...", atomic_load(&sum), expected);
    TEST_ASSERT(atomic_load(&consumed) == total, "count mismatch");
    TEST_ASSERT(atomic_load(&sum) == expected, "sum mismatch");
    
    AnamQueueStats stats;
    anam_queue_stats(q, &stats);
    printf(" (aba_prevented=%zu)...", stats.aba_prevented);
    
    anam_queue_destroy(q);
    TEST_PASS();
}

int main(void) {
    printf("=== Anamnesis Queue Tests ===\n\n");
    test_queue_basic();
    test_queue_concurrent();
    printf("\n=== ALL QUEUE TESTS PASSED ===\n");
    return 0;
}
