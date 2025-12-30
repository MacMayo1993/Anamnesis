/**
 * Anamnesis Tracing Infrastructure
 *
 * Production-grade lock-free tracing for analyzing allocation patterns
 * in concurrent memory pools. Designed to detect phase transitions in
 * slot reuse entropy under varying contention levels.
 *
 * Design:
 *   - Per-thread ring buffers (zero contention)
 *   - Binary format (minimal overhead)
 *   - Compile-time enable/disable (zero cost when off)
 *   - Automatic flush on buffer fill
 *
 * Usage:
 *   #define ANAM_TRACE_ENABLED
 *   #include "anamnesis_trace.h"
 *
 *   anam_trace_init("./traces", 1024 * 1024);
 *   // ... run workload ...
 *   anam_trace_shutdown();
 */

#ifndef ANAMNESIS_TRACE_H
#define ANAMNESIS_TRACE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * TRACE FORMAT
 * ============================================================================ */

/**
 * Trace entry format (16 bytes, cache-line friendly)
 *
 * Binary layout optimized for:
 *   - Fast writes (no serialization overhead)
 *   - Post-processing (struct-aligned reads)
 *   - Analysis (timestamp ordering, slot grouping)
 */
typedef struct AnamTraceEntry {
    uint64_t timestamp;      /* CPU cycle counter (rdtsc/cntvct) */
    uint32_t slot_index;     /* Which slot was operated on */
    uint16_t generation;     /* Generation at operation time */
    uint8_t  op_type;        /* Operation type (see below) */
    uint8_t  thread_id;      /* Thread identifier (0-255) */
} AnamTraceEntry;

/* Operation types */
#define ANAM_TRACE_OP_ALLOC           0  /* Successful allocation */
#define ANAM_TRACE_OP_RELEASE         1  /* Slot released */
#define ANAM_TRACE_OP_GET_VALID       2  /* anam_get() with valid generation */
#define ANAM_TRACE_OP_GET_STALE       3  /* anam_get() with stale generation */
#define ANAM_TRACE_OP_VALIDATE_FAIL   4  /* anam_validate() failed */

#ifdef ANAM_TRACE_ENABLED

/* ============================================================================
 * PUBLIC API (enabled)
 * ============================================================================ */

/**
 * Initialize tracing system.
 *
 * @param output_dir   Directory for trace files (created if needed)
 * @param buffer_size  Per-thread buffer size (must be power of 2)
 *
 * Creates trace_thread_NNN.bin files in output_dir.
 * Call this before creating any threads that will use the pool.
 */
void anam_trace_init(const char* output_dir, uint32_t buffer_size);

/**
 * Shutdown tracing and flush all buffers.
 *
 * Flushes the current thread's buffer and marks system inactive.
 * Other threads should call anam_trace_flush_thread() before exit.
 */
void anam_trace_shutdown(void);

/**
 * Record an allocation event.
 *
 * @param slot_index  Index of allocated slot
 * @param generation  Generation of allocated handle
 */
void anam_trace_alloc(uint32_t slot_index, uint16_t generation);

/**
 * Record a release event.
 *
 * @param slot_index  Index of released slot
 * @param generation  Generation of released handle
 */
void anam_trace_release(uint32_t slot_index, uint16_t generation);

/**
 * Record a get/validation event.
 *
 * @param slot_index  Index accessed
 * @param generation  Claimed generation
 * @param validated   True if generation matched
 */
void anam_trace_get(uint32_t slot_index, uint16_t generation, bool validated);

/**
 * Flush current thread's trace buffer to disk.
 *
 * Should be called by worker threads before pthread_exit().
 * Automatically called by anam_trace_shutdown() for main thread.
 */
void anam_trace_flush_thread(void);

/**
 * Get trace statistics for current thread.
 *
 * @param entries_written  [out] Total entries written
 * @param buffer_overflows [out] Number of overflow events
 *
 * @return true if thread has active trace buffer
 */
bool anam_trace_get_stats(uint64_t* entries_written, uint32_t* buffer_overflows);

#else  /* ANAM_TRACE_ENABLED not defined */

/* ============================================================================
 * PUBLIC API (disabled - zero overhead)
 * ============================================================================ */

#define anam_trace_init(dir, size)                  ((void)0)
#define anam_trace_shutdown()                       ((void)0)
#define anam_trace_alloc(slot, gen)                 ((void)0)
#define anam_trace_release(slot, gen)               ((void)0)
#define anam_trace_get(slot, gen, validated)        ((void)0)
#define anam_trace_flush_thread()                   ((void)0)
#define anam_trace_get_stats(written, overflows)    (false)

#endif  /* ANAM_TRACE_ENABLED */

#ifdef __cplusplus
}
#endif

#endif  /* ANAMNESIS_TRACE_H */
