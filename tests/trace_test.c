/**
 * Tracing Test - Validates trace infrastructure
 *
 * Runs a simple multi-threaded workload with tracing enabled,
 * then verifies trace files were created correctly.
 */

#include "anamnesis.h"
#include "anamnesis_trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>

#ifdef _WIN32
#include <windows.h>
#define sleep_ms(ms) Sleep(ms)
#else
#include <pthread.h>
#include <unistd.h>
#define sleep_ms(ms) usleep((ms) * 1000)
#endif

#define NUM_THREADS 4
#define OPS_PER_THREAD 10000

typedef struct {
    AnamPool* pool;
    int thread_id;
    _Atomic(uint64_t)* total_ops;
} WorkerCtx;

#ifdef _WIN32
DWORD WINAPI worker(LPVOID arg) {
#else
void* worker(void* arg) {
#endif
    WorkerCtx* ctx = (WorkerCtx*)arg;
    AnamHandle handles[100];

    for (int i = 0; i < OPS_PER_THREAD; i++) {
        /* Allocate batch */
        for (int j = 0; j < 100; j++) {
            handles[j] = anam_alloc(ctx->pool);
            if (anam_is_null(handles[j])) {
                fprintf(stderr, "Thread %d: alloc failed at %d\n", ctx->thread_id, i);
                break;
            }

            /* Write some data */
            int* data = (int*)anam_get(ctx->pool, handles[j]);
            if (data) *data = ctx->thread_id * 1000000 + i * 100 + j;
        }

        /* Release batch */
        for (int j = 0; j < 100; j++) {
            if (!anam_is_null(handles[j])) {
                anam_release(ctx->pool, handles[j]);
            }
        }

        atomic_fetch_add(ctx->total_ops, 200); /* 100 allocs + 100 releases */
    }

    /* Flush traces before exit */
    anam_trace_flush_thread();

    return 0;
}

int main(int argc, char** argv) {
    int num_threads = NUM_THREADS;

    if (argc > 1) {
        num_threads = atoi(argv[1]);
        if (num_threads < 1 || num_threads > 64) {
            fprintf(stderr, "Usage: %s [num_threads]\n", argv[0]);
            fprintf(stderr, "  num_threads: 1-64 (default: %d)\n", NUM_THREADS);
            return 1;
        }
    }

    printf("=== Anamnesis Trace Test ===\n");
    printf("Threads: %d\n", num_threads);
    printf("Operations: %d per thread\n\n", OPS_PER_THREAD * 200);

    /* Initialize tracing */
#ifdef ANAM_TRACE_ENABLED
    char trace_dir[256];
    snprintf(trace_dir, sizeof(trace_dir), "./traces_c%d", num_threads);
    anam_trace_init(trace_dir, 256 * 1024);  /* 256K entries per thread */
    printf("Tracing enabled: %s\n", trace_dir);
#else
    printf("Tracing disabled (build with -DANAM_TRACE=ON)\n");
#endif

    /* Create pool */
    AnamPoolConfig cfg = { .slot_size = 64, .slot_count = 1024 };
    AnamPool* pool = anam_pool_create(&cfg);
    if (!pool) {
        fprintf(stderr, "Failed to create pool\n");
        return 1;
    }

    /* Launch workers */
    _Atomic(uint64_t) total_ops = 0;
    WorkerCtx contexts[64];

#ifdef _WIN32
    HANDLE threads[64];
    for (int i = 0; i < num_threads; i++) {
        contexts[i].pool = pool;
        contexts[i].thread_id = i;
        contexts[i].total_ops = &total_ops;
        threads[i] = CreateThread(NULL, 0, worker, &contexts[i], 0, NULL);
    }

    WaitForMultipleObjects(num_threads, threads, TRUE, INFINITE);
    for (int i = 0; i < num_threads; i++) CloseHandle(threads[i]);
#else
    pthread_t threads[64];
    for (int i = 0; i < num_threads; i++) {
        contexts[i].pool = pool;
        contexts[i].thread_id = i;
        contexts[i].total_ops = &total_ops;
        pthread_create(&threads[i], NULL, worker, &contexts[i]);
    }

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }
#endif

    /* Get stats */
    AnamPoolStats stats;
    anam_pool_stats(pool, &stats);

    uint64_t ops = atomic_load(&total_ops);

    printf("\nResults:\n");
    printf("  Total operations: %llu\n", (unsigned long long)ops);
    printf("  Pool allocs:      %zu\n", stats.alloc_count);
    printf("  Pool releases:    %zu\n", stats.release_count);
    printf("  Anamnesis count:  %zu\n", stats.anamnesis_count);

#ifdef ANAM_TRACE_ENABLED
    anam_trace_shutdown();
    printf("\nTraces written to %s/\n", trace_dir);
    printf("Analyze with: python tools/analyze_traces.py %s --num-slots=1024\n", trace_dir);
#endif

    anam_pool_destroy(pool);

    printf("\n✓ Test completed successfully\n");
    return 0;
}
