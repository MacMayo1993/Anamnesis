/**
 * Anamnesis - Unit Tests
 * 
 * Testing the moment of truth: does anamnesis expose the counterfeit?
 */

#include "anamnesis.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n  at %s:%d\n", msg, __FILE__, __LINE__); \
        exit(1); \
    } \
} while(0)

#define TEST_BEGIN(name) printf("[Test] %s...", name); fflush(stdout)
#define TEST_PASS() printf(" PASSED\n")

/* ============================================================================
 * BASIC TESTS
 * ============================================================================ */

void test_create_destroy(void) {
    TEST_BEGIN("create_destroy");
    
    AnamPoolConfig cfg = ANAM_POOL_DEFAULT;
    AnamPool* pool = anam_pool_create(&cfg);
    TEST_ASSERT(pool != NULL, "pool creation failed");
    
    AnamPoolStats stats;
    anam_pool_stats(pool, &stats);
    TEST_ASSERT(stats.slot_count == 1024, "wrong slot count");
    TEST_ASSERT(stats.slots_free == 1024, "wrong free count");
    TEST_ASSERT(stats.anamnesis_count == 0, "should have no anamnesis events yet");
    
    anam_pool_destroy(pool);
    TEST_PASS();
}

void test_alloc_release(void) {
    TEST_BEGIN("alloc_release");
    
    AnamPoolConfig cfg = { .slot_size = 64, .slot_count = 10 };
    AnamPool* pool = anam_pool_create(&cfg);
    
    AnamHandle handles[10];
    for (int i = 0; i < 10; i++) {
        handles[i] = anam_alloc(pool);
        TEST_ASSERT(!anam_is_null(handles[i]), "allocation failed");
    }
    
    /* Pool should be exhausted */
    AnamHandle extra = anam_alloc(pool);
    TEST_ASSERT(anam_is_null(extra), "should be exhausted");
    
    /* Release one */
    bool released = anam_release(pool, handles[0]);
    TEST_ASSERT(released, "release failed");
    
    /* Reallocate */
    AnamHandle realloc = anam_alloc(pool);
    TEST_ASSERT(!anam_is_null(realloc), "realloc failed");
    TEST_ASSERT(anam_generation(realloc) == 1, "generation should increment");
    
    anam_pool_destroy(pool);
    TEST_PASS();
}

/* ============================================================================
 * ANAMNESIS TESTS — EXPOSING THE COUNTERFEIT
 * ============================================================================ */

void test_stale_handle_exposed(void) {
    TEST_BEGIN("stale_handle_exposed (anamnesis)");
    
    AnamPoolConfig cfg = { .slot_size = 64, .slot_count = 10 };
    AnamPool* pool = anam_pool_create(&cfg);
    
    AnamHandle h = anam_alloc(pool);
    TEST_ASSERT(anam_validate(pool, h), "valid handle should validate");
    
    /* Release — the handle becomes counterfeit */
    anam_release(pool, h);
    
    /* The moment of anamnesis: the counterfeit is exposed */
    TEST_ASSERT(!anam_validate(pool, h), "stale handle MUST be exposed");
    TEST_ASSERT(anam_get(pool, h) == NULL, "stale handle MUST return NULL");
    
    AnamPoolStats stats;
    anam_pool_stats(pool, &stats);
    TEST_ASSERT(stats.anamnesis_count >= 1, "anamnesis event should be recorded");
    
    anam_pool_destroy(pool);
    TEST_PASS();
}

void test_aba_prevention(void) {
    TEST_BEGIN("aba_prevention (same address, different generation)");
    
    AnamPoolConfig cfg = { .slot_size = 64, .slot_count = 10 };
    AnamPool* pool = anam_pool_create(&cfg);
    
    /* Allocate */
    AnamHandle h1 = anam_alloc(pool);
    void* addr1 = anam_get(pool, h1);
    uint16_t gen1 = anam_generation(h1);
    
    /* Release and reallocate — likely same address */
    anam_release(pool, h1);
    AnamHandle h2 = anam_alloc(pool);
    void* addr2 = anam_get(pool, h2);
    uint16_t gen2 = anam_generation(h2);
    
    if (addr1 == addr2) {
        /* Same address, but generation must differ */
        TEST_ASSERT(gen2 == gen1 + 1, "generation MUST increment");
        TEST_ASSERT(h1 != h2, "handles MUST differ");
        
        /* Old handle is counterfeit — anamnesis exposes it */
        TEST_ASSERT(!anam_validate(pool, h1), "old handle MUST be exposed");
        TEST_ASSERT(anam_get(pool, h1) == NULL, "old handle MUST return NULL");
    }
    
    /* New handle is valid */
    TEST_ASSERT(anam_validate(pool, h2), "new handle MUST be valid");
    
    anam_pool_destroy(pool);
    TEST_PASS();
}

void test_double_release(void) {
    TEST_BEGIN("double_release (counterfeit attempt)");
    
    AnamPoolConfig cfg = { .slot_size = 64, .slot_count = 10 };
    AnamPool* pool = anam_pool_create(&cfg);
    
    AnamHandle h = anam_alloc(pool);
    
    /* First release succeeds */
    TEST_ASSERT(anam_release(pool, h), "first release should succeed");
    
    /* Second release: the handle is now counterfeit */
    TEST_ASSERT(!anam_release(pool, h), "double release MUST be exposed");
    
    AnamPoolStats stats;
    anam_pool_stats(pool, &stats);
    TEST_ASSERT(stats.anamnesis_count >= 1, "anamnesis event should be recorded");
    
    anam_pool_destroy(pool);
    TEST_PASS();
}

/* ============================================================================
 * DATA INTEGRITY
 * ============================================================================ */

void test_data_integrity(void) {
    TEST_BEGIN("data_integrity");
    
    typedef struct { int x; int y; char name[32]; } TestObj;
    
    AnamPoolConfig cfg = { .slot_size = sizeof(TestObj), .slot_count = 100 };
    AnamPool* pool = anam_pool_create(&cfg);
    
    AnamHandle handles[100];
    for (int i = 0; i < 100; i++) {
        handles[i] = anam_alloc(pool);
        TestObj* obj = ANAM_GET(pool, handles[i], TestObj);
        obj->x = i;
        obj->y = i * 2;
        snprintf(obj->name, sizeof(obj->name), "Object_%d", i);
    }
    
    /* Verify */
    for (int i = 0; i < 100; i++) {
        TestObj* obj = ANAM_GET(pool, handles[i], TestObj);
        TEST_ASSERT(obj->x == i, "x mismatch");
        TEST_ASSERT(obj->y == i * 2, "y mismatch");
    }
    
    anam_pool_destroy(pool);
    TEST_PASS();
}

/* ============================================================================
 * CONCURRENT TEST
 * ============================================================================ */

#define NUM_THREADS 8
#define OPS_PER_THREAD 10000

typedef struct {
    AnamPool* pool;
    atomic_int success;
} ThreadCtx;

#ifdef _WIN32
DWORD WINAPI worker(LPVOID arg) {
#else
void* worker(void* arg) {
#endif
    ThreadCtx* ctx = (ThreadCtx*)arg;
    
    for (int i = 0; i < OPS_PER_THREAD; i++) {
        AnamHandle h = anam_alloc(ctx->pool);
        if (!anam_is_null(h)) {
            int* data = (int*)anam_get(ctx->pool, h);
            if (data) {
                *data = i;
                if (*data == i && anam_validate(ctx->pool, h)) {
                    atomic_fetch_add(&ctx->success, 1);
                }
            }
            anam_release(ctx->pool, h);
        }
    }
    return 0;
}

void test_concurrent(void) {
    TEST_BEGIN("concurrent");
    
    AnamPoolConfig cfg = { .slot_size = 64, .slot_count = 1000 };
    AnamPool* pool = anam_pool_create(&cfg);
    
    ThreadCtx ctx = { .pool = pool };
    atomic_store(&ctx.success, 0);
    
#ifdef _WIN32
    HANDLE threads[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++)
        threads[i] = CreateThread(NULL, 0, worker, &ctx, 0, NULL);
    WaitForMultipleObjects(NUM_THREADS, threads, TRUE, INFINITE);
    for (int i = 0; i < NUM_THREADS; i++)
        CloseHandle(threads[i]);
#else
    pthread_t threads[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++)
        pthread_create(&threads[i], NULL, worker, &ctx);
    for (int i = 0; i < NUM_THREADS; i++)
        pthread_join(threads[i], NULL);
#endif
    
    int success = atomic_load(&ctx.success);
    printf(" (%d ops)...", success);
    TEST_ASSERT(success > NUM_THREADS * OPS_PER_THREAD / 2, "too many failures");
    
    AnamPoolStats stats;
    anam_pool_stats(pool, &stats);
    TEST_ASSERT(stats.slots_free == 1000, "all slots should be free");
    
    anam_pool_destroy(pool);
    TEST_PASS();
}

/* ============================================================================
 * GENERATION CYCLING
 * ============================================================================ */

void test_generation_cycling(void) {
    TEST_BEGIN("generation_cycling");
    
    /* Single slot, cycle many times */
    AnamPoolConfig cfg = { .slot_size = 64, .slot_count = 1 };
    AnamPool* pool = anam_pool_create(&cfg);
    
    AnamHandle old_handles[100];
    
    for (int i = 0; i < 100; i++) {
        AnamHandle h = anam_alloc(pool);
        TEST_ASSERT(!anam_is_null(h), "alloc should succeed");
        TEST_ASSERT(anam_generation(h) == (uint16_t)i, "generation should match cycle");
        
        old_handles[i] = h;
        anam_release(pool, h);
    }
    
    /* All old handles should be counterfeit */
    for (int i = 0; i < 99; i++) {
        TEST_ASSERT(!anam_validate(pool, old_handles[i]), "old handle must be exposed");
    }
    
    AnamPoolStats stats;
    anam_pool_stats(pool, &stats);
    TEST_ASSERT(stats.generation_max == 99, "max generation should be 99");
    
    anam_pool_destroy(pool);
    TEST_PASS();
}

/* ============================================================================
 * MAIN
 * ============================================================================ */

int main(void) {
    printf("=== Anamnesis Unit Tests ===\n");
    printf("\"The Empire never ended.\"\n\n");
    
    test_create_destroy();
    test_alloc_release();
    test_stale_handle_exposed();
    test_aba_prevention();
    test_double_release();
    test_data_integrity();
    test_concurrent();
    test_generation_cycling();
    
    printf("\n=== ALL TESTS PASSED ===\n");
    printf("The counterfeit was exposed.\n");
    return 0;
}
