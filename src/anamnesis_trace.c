/**
 * Anamnesis Tracing - Implementation
 */

#include "anamnesis_trace.h"

#ifdef ANAM_TRACE_ENABLED

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#else
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

/* ============================================================================
 * PLATFORM-SPECIFIC TIMESTAMPS
 * ============================================================================ */

static inline uint64_t anam_rdtsc(void) {
#if defined(__x86_64__) || defined(__i386__)
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__)
    uint64_t val;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(val));
    return val;
#else
    /* Fallback: nanosecond precision */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
#endif
}

/* ============================================================================
 * TRACE BUFFER
 * ============================================================================ */

typedef struct AnamTraceBuffer {
    AnamTraceEntry* entries;
    uint32_t capacity;       /* Must be power of 2 */
    uint32_t head;           /* Write position */
    uint32_t tail;           /* Flush position */
    uint8_t thread_id;
    uint32_t overflow_count;
    uint64_t entries_written;
    bool active;
} AnamTraceBuffer;

/* Global context */
typedef struct {
    char output_dir[256];
    uint32_t buffer_capacity;
    _Atomic uint32_t next_thread_id;
    bool active;
} AnamTraceContext;

static AnamTraceContext g_trace_ctx = {0};

/* Thread-local buffer */
#ifdef _WIN32
static __declspec(thread) AnamTraceBuffer* tls_buffer = NULL;
#else
static __thread AnamTraceBuffer* tls_buffer = NULL;
#endif

/* ============================================================================
 * BUFFER MANAGEMENT
 * ============================================================================ */

static AnamTraceBuffer* anam_trace_buffer_create(uint32_t capacity) {
    /* Validate capacity is power of 2 */
    if (capacity == 0 || (capacity & (capacity - 1)) != 0) {
        fprintf(stderr, "[anam_trace] Buffer capacity must be power of 2, got %u\n", capacity);
        return NULL;
    }

    AnamTraceBuffer* buf = (AnamTraceBuffer*)calloc(1, sizeof(AnamTraceBuffer));
    if (!buf) {
        fprintf(stderr, "[anam_trace] Failed to allocate trace buffer\n");
        return NULL;
    }

    buf->entries = (AnamTraceEntry*)calloc(capacity, sizeof(AnamTraceEntry));
    if (!buf->entries) {
        fprintf(stderr, "[anam_trace] Failed to allocate %u trace entries\n", capacity);
        free(buf);
        return NULL;
    }

    buf->capacity = capacity;
    buf->head = 0;
    buf->tail = 0;
    buf->overflow_count = 0;
    buf->entries_written = 0;
    buf->active = true;
    buf->thread_id = (uint8_t)atomic_fetch_add(&g_trace_ctx.next_thread_id, 1);

    return buf;
}

static void anam_trace_buffer_destroy(AnamTraceBuffer* buf) {
    if (!buf) return;
    free(buf->entries);
    free(buf);
}

static void anam_trace_buffer_flush(AnamTraceBuffer* buf) {
    if (!buf || buf->head == buf->tail) return;

    char filename[512];
    snprintf(filename, sizeof(filename), "%s/trace_thread_%03d.bin",
             g_trace_ctx.output_dir, buf->thread_id);

    FILE* f = fopen(filename, "ab");
    if (!f) {
        fprintf(stderr, "[anam_trace] Failed to open %s: %s\n",
                filename, strerror(errno));
        return;
    }

    /* Calculate entries to flush */
    uint32_t mask = buf->capacity - 1;
    uint32_t count = (buf->head - buf->tail) & mask;

    /* Handle empty buffer (but check overflow flag) */
    if (count == 0 && buf->overflow_count > 0) {
        count = buf->capacity;  /* Buffer wrapped, flush all */
    }

    if (count == 0) {
        fclose(f);
        return;
    }

    /* Write entries (handle ring buffer wrap) */
    uint32_t tail_idx = buf->tail & mask;
    uint32_t head_idx = buf->head & mask;

    if (tail_idx < head_idx) {
        /* Contiguous block */
        size_t written = fwrite(&buf->entries[tail_idx], sizeof(AnamTraceEntry),
                                count, f);
        if (written != count) {
            fprintf(stderr, "[anam_trace] Incomplete write: %zu/%u entries\n",
                    written, count);
        }
    } else {
        /* Wrapped: write tail to end, then beginning to head */
        uint32_t first_chunk = buf->capacity - tail_idx;
        size_t written1 = fwrite(&buf->entries[tail_idx], sizeof(AnamTraceEntry),
                                 first_chunk, f);
        size_t written2 = fwrite(&buf->entries[0], sizeof(AnamTraceEntry),
                                 count - first_chunk, f);

        if (written1 + written2 != count) {
            fprintf(stderr, "[anam_trace] Incomplete wrapped write: %zu/%u entries\n",
                    written1 + written2, count);
        }
    }

    fclose(f);

    /* Update tail */
    buf->tail = buf->head;
}

/* ============================================================================
 * TRACE RECORDING
 * ============================================================================ */

static AnamTraceBuffer* anam_trace_get_buffer(void) {
    if (!tls_buffer && g_trace_ctx.active) {
        tls_buffer = anam_trace_buffer_create(g_trace_ctx.buffer_capacity);
    }
    return tls_buffer;
}

static inline void anam_trace_record(uint8_t op_type, uint32_t slot_index,
                                     uint16_t generation) {
    AnamTraceBuffer* buf = anam_trace_get_buffer();
    if (!buf || !buf->active) return;

    uint32_t mask = buf->capacity - 1;
    uint32_t idx = buf->head & mask;

    buf->entries[idx] = (AnamTraceEntry){
        .timestamp = anam_rdtsc(),
        .slot_index = slot_index,
        .generation = generation,
        .op_type = op_type,
        .thread_id = buf->thread_id
    };

    buf->head++;
    buf->entries_written++;

    /* Check if buffer is getting full (flush at 75% to avoid overflow) */
    uint32_t used = (buf->head - buf->tail) & mask;
    if (used > (buf->capacity * 3 / 4)) {
        anam_trace_buffer_flush(buf);
    }

    /* Detect overflow */
    if (buf->head - buf->tail >= buf->capacity) {
        buf->overflow_count++;
        /* Note: In overflow, oldest entries are lost (ring buffer behavior) */
    }
}

/* ============================================================================
 * PUBLIC API
 * ============================================================================ */

void anam_trace_init(const char* output_dir, uint32_t buffer_size) {
    if (!output_dir || buffer_size == 0) {
        fprintf(stderr, "[anam_trace] Invalid parameters to anam_trace_init\n");
        return;
    }

    /* Validate buffer size is power of 2 */
    if ((buffer_size & (buffer_size - 1)) != 0) {
        fprintf(stderr, "[anam_trace] Buffer size must be power of 2, got %u\n",
                buffer_size);
        return;
    }

    strncpy(g_trace_ctx.output_dir, output_dir,
            sizeof(g_trace_ctx.output_dir) - 1);
    g_trace_ctx.output_dir[sizeof(g_trace_ctx.output_dir) - 1] = '\0';
    g_trace_ctx.buffer_capacity = buffer_size;
    atomic_store(&g_trace_ctx.next_thread_id, 0);
    g_trace_ctx.active = true;

    /* Create output directory */
    if (mkdir(output_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "[anam_trace] Warning: Failed to create %s: %s\n",
                output_dir, strerror(errno));
        /* Continue anyway - might already exist */
    }

    fprintf(stderr, "[anam_trace] Initialized: dir=%s, buffer=%u entries\n",
            output_dir, buffer_size);
}

void anam_trace_shutdown(void) {
    g_trace_ctx.active = false;
    anam_trace_flush_thread();

    fprintf(stderr, "[anam_trace] Shutdown complete\n");
}

void anam_trace_alloc(uint32_t slot_index, uint16_t generation) {
    anam_trace_record(ANAM_TRACE_OP_ALLOC, slot_index, generation);
}

void anam_trace_release(uint32_t slot_index, uint16_t generation) {
    anam_trace_record(ANAM_TRACE_OP_RELEASE, slot_index, generation);
}

void anam_trace_get(uint32_t slot_index, uint16_t generation, bool validated) {
    uint8_t op_type = validated ? ANAM_TRACE_OP_GET_VALID : ANAM_TRACE_OP_GET_STALE;
    anam_trace_record(op_type, slot_index, generation);
}

void anam_trace_flush_thread(void) {
    if (tls_buffer) {
        if (tls_buffer->active) {
            anam_trace_buffer_flush(tls_buffer);
            tls_buffer->active = false;
        }
        anam_trace_buffer_destroy(tls_buffer);
        tls_buffer = NULL;
    }
}

bool anam_trace_get_stats(uint64_t* entries_written, uint32_t* buffer_overflows) {
    if (!tls_buffer) return false;

    if (entries_written) *entries_written = tls_buffer->entries_written;
    if (buffer_overflows) *buffer_overflows = tls_buffer->overflow_count;

    return true;
}

#endif  /* ANAM_TRACE_ENABLED */
