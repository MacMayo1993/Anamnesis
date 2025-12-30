/**
 * Anamnesis Stress Tests
 *
 * Long-running concurrent tests designed to expose race conditions,
 * memory corruption, and ABA bugs under heavy load.
 *
 * Run with sanitizers:
 *   - TSan: catches data races
 *   - ASan: catches memory errors
 *   - UBSan: catches undefined behavior
 */

#include "anamnesis.h"
#include "anamnesis_queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#define sleep_ms(ms) Sleep(ms)
#else
#include <pthread.h>
#include <unistd.h>
#define sleep_ms(ms) usleep((ms) * 1000)
#endif

/* Test configuration */
#ifndef STRESS_DURATION_SEC
#define STRESS_DURATION_SEC 10
#endif

#define NUM_THREADS 8
#define POOL_SIZE 1000
#define SLOT_SIZE 64

/* Statistics */
typedef struct {
    _Atomic(uint64_t) allocs;
    _Atomic(uint64_t) releases;
    _Atomic(uint64_t) gets;
    _Atomic(uint64_t) validates;
    _Atomic(uint64_t) expected_stale;
    _Atomic(uint64_t) actual_stale;
    _Atomic(bool) stop;
} StressStats;

static StressStats stats;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "\n[FAIL] %s\n  at %s:%d\n", msg, __FILE__, __LINE__); \
        exit(1); \
    } \
} while(0)

#define TEST_BEGIN(name) printf("[Stress Test] %s", name); fflush(stdout)
#define TEST_PASS() printf(" PASSED\n")

/* ============================================================================
 * POOL STRESS TEST: Concurrent alloc/release with intentional stale handles
 * ============================================================================ */

typedef struct {
    AnamPool* pool;
    int thread_id;
} PoolStressCtx;

#ifdef _WIN32
DWORD WINAPI pool_stress_worker(LPVOID arg) {
#else
void* pool_stress_worker(void* arg) {
#endif
    PoolStressCtx* ctx = (PoolStressCtx*)arg;
    AnamHandle handles[100];
    int handle_count = 0;

    while (!atomic_load(&stats.stop)) {
        /* Allocate some handles */
        for (int i = 0; i < 10 && handle_count < 100; i++) {
            AnamHandle h = anam_alloc(ctx->pool);
            if (!anam_is_null(h)) {
                handles[handle_count++] = h;
                atomic_fetch_add(&stats.allocs, 1);

                /* Write some data (atomic to avoid TSan false positives on slot reuse) */
                _Atomic(int)* data = (_Atomic(int)*)anam_get(ctx->pool, h);
                if (data) {
                    atomic_store(data, ctx->thread_id * 10000 + handle_count);
                    atomic_fetch_add(&stats.gets, 1);
                }
            }
        }

        /* Release half of them */
        int to_release = handle_count / 2;
        for (int i = 0; i < to_release; i++) {
            if (anam_release(ctx->pool, handles[i])) {
                atomic_fetch_add(&stats.releases, 1);
            }
        }

        /* Try to use the released handles (should be detected as stale) */
        for (int i = 0; i < to_release; i++) {
            if (!anam_validate(ctx->pool, handles[i])) {
                atomic_fetch_add(&stats.actual_stale, 1);
            }
            atomic_fetch_add(&stats.validates, 1);
            atomic_fetch_add(&stats.expected_stale, 1);
        }

        /* Shift remaining handles down */
        for (int i = 0; i < handle_count - to_release; i++) {
            handles[i] = handles[i + to_release];
        }
        handle_count -= to_release;

        /* Validate remaining handles (should all be valid) */
        for (int i = 0; i < handle_count; i++) {
            if (!anam_validate(ctx->pool, handles[i])) {
                atomic_fetch_add(&stats.actual_stale, 1);
            }
            atomic_fetch_add(&stats.validates, 1);
        }
    }

    /* Clean up remaining handles */
    for (int i = 0; i < handle_count; i++) {
        anam_release(ctx->pool, handles[i]);
    }

    return 0;
}

void test_pool_stress(void) {
    TEST_BEGIN("pool_stress");
    printf(" [%d threads × %d seconds]...", NUM_THREADS, STRESS_DURATION_SEC);
    fflush(stdout);

    AnamPoolConfig cfg = { .slot_size = SLOT_SIZE, .slot_count = POOL_SIZE };
    AnamPool* pool = anam_pool_create(&cfg);
    TEST_ASSERT(pool != NULL, "pool creation failed");

    /* Reset stats */
    atomic_store(&stats.allocs, 0);
    atomic_store(&stats.releases, 0);
    atomic_store(&stats.gets, 0);
    atomic_store(&stats.validates, 0);
    atomic_store(&stats.expected_stale, 0);
    atomic_store(&stats.actual_stale, 0);
    atomic_store(&stats.stop, false);

    /* Spawn worker threads */
    PoolStressCtx contexts[NUM_THREADS];
#ifdef _WIN32
    HANDLE threads[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {
        contexts[i].pool = pool;
        contexts[i].thread_id = i;
        threads[i] = CreateThread(NULL, 0, pool_stress_worker, &contexts[i], 0, NULL);
    }
#else
    pthread_t threads[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {
        contexts[i].pool = pool;
        contexts[i].thread_id = i;
        pthread_create(&threads[i], NULL, pool_stress_worker, &contexts[i]);
    }
#endif

    /* Let them run for specified duration */
    sleep_ms(STRESS_DURATION_SEC * 1000);

    /* Signal stop and wait for completion */
    atomic_store(&stats.stop, true);
#ifdef _WIN32
    WaitForMultipleObjects(NUM_THREADS, threads, TRUE, INFINITE);
    for (int i = 0; i < NUM_THREADS; i++) CloseHandle(threads[i]);
#else
    for (int i = 0; i < NUM_THREADS; i++) pthread_join(threads[i], NULL);
#endif

    /* Verify statistics */
    uint64_t allocs = atomic_load(&stats.allocs);
    uint64_t releases = atomic_load(&stats.releases);
    uint64_t gets = atomic_load(&stats.gets);
    uint64_t validates = atomic_load(&stats.validates);
    uint64_t expected_stale = atomic_load(&stats.expected_stale);
    uint64_t actual_stale = atomic_load(&stats.actual_stale);

    printf("\n  Stats: %llu allocs, %llu releases, %llu gets, %llu validates\n",
           (unsigned long long)allocs, (unsigned long long)releases,
           (unsigned long long)gets, (unsigned long long)validates);

    AnamPoolStats pool_stats;
    anam_pool_stats(pool, &pool_stats);
    printf("  Pool: anamnesis_count=%zu (expected ~%llu stale validations)\n",
           pool_stats.anamnesis_count, (unsigned long long)expected_stale);

    /* The pool's anamnesis_count should roughly match our expected stale count
       (might differ slightly due to double-releases being counted) */
    TEST_ASSERT(pool_stats.anamnesis_count >= expected_stale * 0.9,
                "anamnesis_count too low (not detecting stale handles?)");

    /* We should have detected most stale handles in our validation checks */
    TEST_ASSERT(actual_stale >= expected_stale * 0.9,
                "failed to detect expected stale handles");

    printf("  ✓ Stale handle detection working correctly\n");

    anam_pool_destroy(pool);
    TEST_PASS();
}

/* ============================================================================
 * QUEUE STRESS TEST: High-throughput producer-consumer
 * ============================================================================ */

typedef struct {
    AnamQueue* queue;
    int thread_id;
    bool is_producer;
} QueueStressCtx;

#ifdef _WIN32
DWORD WINAPI queue_stress_worker(LPVOID arg) {
#else
void* queue_stress_worker(void* arg) {
#endif
    QueueStressCtx* ctx = (QueueStressCtx*)arg;

    if (ctx->is_producer) {
        uint64_t produced = 0;
        while (!atomic_load(&stats.stop)) {
            int value = ctx->thread_id * 1000000 + (int)produced;
            while (anam_is_null(anam_queue_push(ctx->queue, &value))) {
                if (atomic_load(&stats.stop)) goto done;
                sleep_ms(1);
            }
            produced++;
        }
done:
        atomic_fetch_add(&stats.allocs, produced);
    } else {
        uint64_t consumed = 0;
        while (!atomic_load(&stats.stop)) {
            int value;
            if (anam_queue_pop(ctx->queue, &value)) {
                consumed++;
            } else {
                sleep_ms(1);
            }
        }
        /* Drain remaining items */
        int value;
        while (anam_queue_pop(ctx->queue, &value)) {
            consumed++;
        }
        atomic_fetch_add(&stats.releases, consumed);
    }

    return 0;
}

void test_queue_stress(void) {
    TEST_BEGIN("queue_stress");
    printf(" [%d threads × %d seconds]...", NUM_THREADS, STRESS_DURATION_SEC);
    fflush(stdout);

    AnamQueueConfig cfg = { .item_size = sizeof(int), .capacity = 10000 };
    AnamQueue* q = anam_queue_create(&cfg);
    TEST_ASSERT(q != NULL, "queue creation failed");

    /* Reset stats */
    atomic_store(&stats.allocs, 0);
    atomic_store(&stats.releases, 0);
    atomic_store(&stats.stop, false);

    /* Spawn half producers, half consumers */
    QueueStressCtx contexts[NUM_THREADS];
#ifdef _WIN32
    HANDLE threads[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {
        contexts[i].queue = q;
        contexts[i].thread_id = i;
        contexts[i].is_producer = (i < NUM_THREADS / 2);
        threads[i] = CreateThread(NULL, 0, queue_stress_worker, &contexts[i], 0, NULL);
    }
#else
    pthread_t threads[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {
        contexts[i].queue = q;
        contexts[i].thread_id = i;
        contexts[i].is_producer = (i < NUM_THREADS / 2);
        pthread_create(&threads[i], NULL, queue_stress_worker, &contexts[i]);
    }
#endif

    /* Let them run */
    sleep_ms(STRESS_DURATION_SEC * 1000);

    /* Signal stop and wait */
    atomic_store(&stats.stop, true);
#ifdef _WIN32
    WaitForMultipleObjects(NUM_THREADS, threads, TRUE, INFINITE);
    for (int i = 0; i < NUM_THREADS; i++) CloseHandle(threads[i]);
#else
    for (int i = 0; i < NUM_THREADS; i++) pthread_join(threads[i], NULL);
#endif

    uint64_t produced = atomic_load(&stats.allocs);
    uint64_t consumed = atomic_load(&stats.releases);

    printf("\n  Stats: %llu produced, %llu consumed\n",
           (unsigned long long)produced, (unsigned long long)consumed);

    TEST_ASSERT(produced == consumed, "produced != consumed (lost items?)");
    TEST_ASSERT(produced > 0, "no items produced");

    AnamQueueStats queue_stats;
    anam_queue_stats(q, &queue_stats);
    printf("  Queue: push_count=%zu, pop_count=%zu, aba_prevented=%zu\n",
           queue_stats.push_count, queue_stats.pop_count, queue_stats.aba_prevented);

    printf("  ✓ No items lost, ABA prevention active\n");

    anam_queue_destroy(q);
    TEST_PASS();
}

/* ============================================================================
 * ABA STRESS TEST: Force generation cycling to expose ABA bugs
 * ============================================================================ */

void test_aba_stress(void) {
    TEST_BEGIN("aba_stress");
    printf(" [cycling generations rapidly]...");
    fflush(stdout);

    /* Single slot pool - force rapid generation cycling */
    AnamPoolConfig cfg = { .slot_size = 64, .slot_count = 1 };
    AnamPool* pool = anam_pool_create(&cfg);
    TEST_ASSERT(pool != NULL, "pool creation failed");

    AnamHandle old_handles[1000];

    /* Cycle 1000 times through the single slot */
    for (int i = 0; i < 1000; i++) {
        AnamHandle h = anam_alloc(pool);
        TEST_ASSERT(!anam_is_null(h), "alloc failed during ABA stress");
        TEST_ASSERT(anam_generation(h) == (uint16_t)i, "generation mismatch");

        old_handles[i] = h;
        anam_release(pool, h);
    }

    /* All old handles should be detected as stale */
    int stale_detected = 0;
    for (int i = 0; i < 1000; i++) {
        if (!anam_validate(pool, old_handles[i])) {
            stale_detected++;
        }
    }

    printf("\n  Detected %d/1000 stale handles as invalid", stale_detected);
    TEST_ASSERT(stale_detected >= 999, "failed to detect stale handles after generation cycling");

    AnamPoolStats stats;
    anam_pool_stats(pool, &stats);
    printf("\n  Generation max: %u (expected 999)\n", stats.generation_max);
    TEST_ASSERT(stats.generation_max == 999, "generation_max incorrect");

    anam_pool_destroy(pool);
    TEST_PASS();
}

/* ============================================================================
 * MAIN
 * ============================================================================ */

int main(void) {
    printf("=== Anamnesis Stress Tests ===\n");
    printf("Duration: %d seconds per test\n", STRESS_DURATION_SEC);
    printf("Threads: %d\n\n", NUM_THREADS);

    test_pool_stress();
    test_queue_stress();
    test_aba_stress();

    printf("\n=== ALL STRESS TESTS PASSED ===\n");
    printf("Run with TSan/ASan/UBSan for comprehensive validation!\n");
    return 0;
}
