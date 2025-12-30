/**
 * Anamnesis Benchmarks
 */

#include "anamnesis.h"
#include <stdio.h>
#include <stdlib.h>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>

using Clock = std::chrono::high_resolution_clock;
using Duration = std::chrono::duration<double, std::milli>;

template<typename T>
void do_not_optimize(T const& value) {
    asm volatile("" : : "r,m"(value) : "memory");
}

void bench_alloc_release(size_t iterations) {
    AnamPoolConfig cfg = { .slot_size = 64, .slot_count = 10000, .alignment = 8, .zero_on_alloc = false, .zero_on_release = false };
    AnamPool* pool = anam_pool_create(&cfg);
    
    auto start = Clock::now();
    for (size_t i = 0; i < iterations; i++) {
        AnamHandle h = anam_alloc(pool);
        do_not_optimize(h);
        anam_release(pool, h);
    }
    auto end = Clock::now();
    
    Duration elapsed = end - start;
    double ns_per_op = (elapsed.count() * 1e6) / iterations;
    printf("  anam_alloc + anam_release: %.1f ns/op\n", ns_per_op);
    
    anam_pool_destroy(pool);
}

void bench_malloc_free(size_t iterations) {
    auto start = Clock::now();
    for (size_t i = 0; i < iterations; i++) {
        void* p = malloc(64);
        do_not_optimize(p);
        free(p);
    }
    auto end = Clock::now();
    
    Duration elapsed = end - start;
    double ns_per_op = (elapsed.count() * 1e6) / iterations;
    printf("  malloc + free: %.1f ns/op\n", ns_per_op);
}

void bench_get(size_t iterations) {
    AnamPoolConfig cfg = { .slot_size = 64, .slot_count = 10000, .alignment = 8, .zero_on_alloc = false, .zero_on_release = false };
    AnamPool* pool = anam_pool_create(&cfg);
    
    std::vector<AnamHandle> handles(10000);
    for (size_t i = 0; i < 10000; i++) {
        handles[i] = anam_alloc(pool);
    }
    
    auto start = Clock::now();
    size_t sum = 0;
    for (size_t i = 0; i < iterations; i++) {
        void* ptr = anam_get(pool, handles[i % 10000]);
        sum += (size_t)ptr;
    }
    do_not_optimize(sum);
    auto end = Clock::now();
    
    Duration elapsed = end - start;
    double ns_per_op = (elapsed.count() * 1e6) / iterations;
    printf("  anam_get (validated access): %.1f ns/op\n", ns_per_op);
    
    for (auto h : handles) anam_release(pool, h);
    anam_pool_destroy(pool);
}

void bench_concurrent(int num_threads, size_t ops_per_thread) {
    AnamPoolConfig cfg = { .slot_size = 64, .slot_count = 10000, .alignment = 8, .zero_on_alloc = false, .zero_on_release = false };
    AnamPool* pool = anam_pool_create(&cfg);
    
    std::atomic<size_t> total_ops{0};
    
    auto worker = [&]() {
        size_t local = 0;
        for (size_t i = 0; i < ops_per_thread; i++) {
            AnamHandle h = anam_alloc(pool);
            if (!anam_is_null(h)) {
                local++;
                anam_release(pool, h);
            }
        }
        total_ops.fetch_add(local);
    };
    
    auto start = Clock::now();
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back(worker);
    }
    for (auto& t : threads) t.join();
    auto end = Clock::now();
    
    Duration elapsed = end - start;
    double ops_per_sec = total_ops.load() / (elapsed.count() / 1000.0);
    printf("  concurrent (%d threads): %.0f ops/sec\n", num_threads, ops_per_sec);
    
    anam_pool_destroy(pool);
}

int main() {
    printf("=== Anamnesis Benchmarks ===\n");
    printf("\"The one who remembers.\"\n\n");
    
    const size_t ITERATIONS = 1000000;
    
    printf("--- Single-Threaded ---\n");
    bench_malloc_free(ITERATIONS);
    bench_alloc_release(ITERATIONS);
    bench_get(ITERATIONS * 10);
    
    printf("\n--- Concurrent ---\n");
    bench_concurrent(1, ITERATIONS);
    bench_concurrent(4, ITERATIONS / 4);
    bench_concurrent(8, ITERATIONS / 8);
    
    printf("\n=== Complete ===\n");
    return 0;
}
