/**
 * Anamnesis - Pool Implementation
 * 
 * Lock-free object pool with generation-based handle validation.
 * 
 * The pool remembers the true generation of every slot. Handles that
 * claim a false generation are exposed as counterfeit.
 */

#include "anamnesis.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <stdbool.h>

#ifdef _WIN32
#include <windows.h>
#define ANAM_ALIGNED_ALLOC(alignment, size) _aligned_malloc((size), (alignment))
#define ANAM_ALIGNED_FREE(ptr) _aligned_free(ptr)
#else
#include <stdlib.h>
#define ANAM_ALIGNED_ALLOC(alignment, size) aligned_alloc((alignment), (size))
#define ANAM_ALIGNED_FREE(ptr) free(ptr)
#endif

/* ============================================================================
 * BIT LAYOUT MASKS
 * 
 * The handle is a 64-bit value encoding three fields:
 *   [63..48] Generation (16 bits) — which incarnation?
 *   [47..3]  Address (45 bits) — where in memory?
 *   [2..0]   State (3 bits) — lifecycle status
 * ============================================================================ */

#define ANAM_ADDR_MASK  0x0000FFFFFFFFFFF8ULL
#define ANAM_GEN_MASK   0xFFFF000000000000ULL
#define ANAM_STATE_MASK 0x0000000000000007ULL

static inline AnamHandle encode_handle(uint16_t gen, void* addr, uint8_t state) {
    return ((uint64_t)gen << 48) | 
           ((uint64_t)(uintptr_t)addr & ANAM_ADDR_MASK) | 
           (state & ANAM_STATE_MASK);
}

static inline void* decode_addr(AnamHandle handle) {
    return (void*)(uintptr_t)(handle & ANAM_ADDR_MASK);
}

static inline uint16_t decode_gen(AnamHandle handle) {
    return (uint16_t)(handle >> 48);
}

/* ============================================================================
 * SLOT HEADER
 * 
 * Each slot has a header storing the true generation and free-list link.
 * The header is placed before the user data region.
 * ============================================================================ */

typedef struct SlotHeader {
    _Atomic(AnamHandle) next;     /* Free-list link (when free) */
    _Atomic(uint16_t) generation; /* The TRUE generation — the pool remembers */
} SlotHeader;

/* ============================================================================
 * POOL STRUCTURE
 * ============================================================================ */

struct AnamPool {
    /* Configuration (immutable) */
    size_t slot_size;
    size_t slot_stride;
    size_t slot_stride_mask; /* For fast modulo when stride is power-of-2; otherwise 0 */
    size_t slot_count;
    size_t alignment;
    bool   zero_on_alloc;
    bool   zero_on_release;
    
    /* Memory */
    void* memory;
    void* slots_base;
    
    /* Free list (lock-free Treiber stack) */
    _Atomic(AnamHandle) free_head;
    
    /* Statistics */
    _Atomic(size_t) slots_free;
    _Atomic(size_t) alloc_count;
    _Atomic(size_t) release_count;
    _Atomic(size_t) anamnesis_count;  /* Counterfeit handles exposed */
    _Atomic(uint16_t) generation_max;
};

/* ============================================================================
 * HELPERS
 * ============================================================================ */

static inline SlotHeader* get_header(void* user_ptr) {
    return (SlotHeader*)((char*)user_ptr - sizeof(SlotHeader));
}

static inline void* get_user_ptr(SlotHeader* header) {
    return (void*)((char*)header + sizeof(SlotHeader));
}

static inline void* slot_from_index(AnamPool* pool, size_t index) {
    return (char*)pool->slots_base + (index * pool->slot_stride);
}

static inline bool in_pool(AnamPool* pool, void* ptr) {
    char* p = (char*)ptr;
    char* base = (char*)pool->slots_base;
    char* end = base + (pool->slot_count * pool->slot_stride);
    if (p < base || p >= end) return false;

#ifdef ANAM_STRICT_VALIDATION
    /* Strict mode: verify pointer is exactly on slot boundary (prevents forged mid-slot pointers)
       WARNING: This has significant performance cost. Only enable for debugging/testing. */
    size_t off = (size_t)(p - base);
    /* Fast path: if stride is power-of-2, use bit masking instead of modulo */
    if (pool->slot_stride_mask) {
        return (off & pool->slot_stride_mask) == 0;
    }
    return (off % pool->slot_stride) == 0;
#else
    return true;
#endif
}

static inline size_t align_up(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

/* ============================================================================
 * LIFECYCLE
 * ============================================================================ */

AnamPool* anam_pool_create(const AnamPoolConfig* config) {
    AnamPoolConfig cfg = ANAM_POOL_DEFAULT;
    if (config) cfg = *config;
    
    if (cfg.slot_size == 0 || cfg.slot_count == 0) return NULL;
    if (cfg.alignment == 0) cfg.alignment = 8;
    if ((cfg.alignment & (cfg.alignment - 1)) != 0) return NULL;
    if (cfg.alignment < 8) return NULL; /* handle encodes 3 state bits in address */
    
    AnamPool* pool = (AnamPool*)calloc(1, sizeof(AnamPool));
    if (!pool) return NULL;
    
    /* Calculate slot layout */
    size_t header_size = align_up(sizeof(SlotHeader), cfg.alignment);
    size_t user_size = align_up(cfg.slot_size, cfg.alignment);
    size_t stride = align_up(header_size + user_size, 8);
    
    pool->slot_size = cfg.slot_size;
    pool->slot_stride = stride;
    pool->slot_stride_mask = (stride & (stride - 1)) == 0 ? (stride - 1) : 0;
    pool->slot_count = cfg.slot_count;
    pool->alignment = cfg.alignment;
    pool->zero_on_alloc = cfg.zero_on_alloc;
    pool->zero_on_release = cfg.zero_on_release;
    
    /* Allocate memory */
    size_t total = stride * cfg.slot_count + cfg.alignment;
    total = align_up(total, cfg.alignment); /* C11 aligned_alloc requires size multiple of alignment */
    pool->memory = ANAM_ALIGNED_ALLOC(cfg.alignment, total);
    if (!pool->memory) {
        free(pool);
        return NULL;
    }
    
    /* Align slots base */
    uintptr_t base = (uintptr_t)pool->memory;
    base = align_up(base + sizeof(SlotHeader), cfg.alignment);
    pool->slots_base = (void*)base;
    
    /* Initialize free list */
    atomic_store(&pool->free_head, ANAM_NULL);
    atomic_store(&pool->slots_free, cfg.slot_count);
    
    /* Push all slots onto free list (reverse order) */
    for (size_t i = cfg.slot_count; i > 0; i--) {
        void* user_ptr = slot_from_index(pool, i - 1);
        SlotHeader* header = get_header(user_ptr);
        
        atomic_store(&header->generation, 0);
        
        AnamHandle old_head = atomic_load(&pool->free_head);
        AnamHandle new_handle = encode_handle(0, user_ptr, ANAM_STATE_FREE);
        atomic_store(&header->next, old_head);
        atomic_store(&pool->free_head, new_handle);
    }
    
    return pool;
}

void anam_pool_destroy(AnamPool* pool) {
    if (!pool) return;
    if (pool->memory) ANAM_ALIGNED_FREE(pool->memory);
    free(pool);
}

/* ============================================================================
 * ALLOCATION
 * ============================================================================ */

AnamHandle anam_alloc(AnamPool* pool) {
    if (!pool) return ANAM_NULL;
    
    AnamHandle old_head, new_head;
    void* user_ptr;
    SlotHeader* header;
    
    do {
        old_head = atomic_load_explicit(&pool->free_head, memory_order_acquire);
        if (anam_is_null(old_head)) return ANAM_NULL;
        
        user_ptr = decode_addr(old_head);
        header = get_header(user_ptr);
        new_head = atomic_load_explicit(&header->next, memory_order_relaxed);
        
    } while (!atomic_compare_exchange_weak_explicit(
        &pool->free_head, &old_head, new_head,
        memory_order_acquire, memory_order_relaxed));

    uint16_t gen = atomic_load(&header->generation);
    uint16_t max_gen = atomic_load(&pool->generation_max);
    while (gen > max_gen) {
        if (atomic_compare_exchange_weak(&pool->generation_max, &max_gen, gen)) break;
    }
    atomic_store(&header->next, ANAM_NULL);
    
    atomic_fetch_sub(&pool->slots_free, 1);
    atomic_fetch_add(&pool->alloc_count, 1);
    
    if (pool->zero_on_alloc) {
        memset(user_ptr, 0, pool->slot_size);
    }
    
    return encode_handle(gen, user_ptr, ANAM_STATE_LIVE);
}

bool anam_release(AnamPool* pool, AnamHandle handle) {
    if (!pool || anam_is_null(handle)) return false;
    if (anam_state(handle) != ANAM_STATE_LIVE) {
        atomic_fetch_add(&pool->anamnesis_count, 1);
        return false;
    }

    void* user_ptr = decode_addr(handle);
    if (!in_pool(pool, user_ptr)) {
        atomic_fetch_add(&pool->anamnesis_count, 1);
        return false;
    }
    
    SlotHeader* header = get_header(user_ptr);
    
    /* Verify generation — is this handle telling the truth? */
    uint16_t claimed_gen = decode_gen(handle);
    uint16_t true_gen = atomic_load(&header->generation);
    
    if (claimed_gen != true_gen) {
        /* Anamnesis: The handle claims a false identity */
        atomic_fetch_add(&pool->anamnesis_count, 1);
        return false;
    }
    
    /* Increment generation — this slot is reborn */
    uint16_t new_gen = true_gen + 1;
    atomic_store(&header->generation, new_gen);
    
    if (pool->zero_on_release) {
        memset(user_ptr, 0, pool->slot_size);
    }
    
    /* Push back onto free list */
    AnamHandle old_head, new_handle;
    do {
        old_head = atomic_load_explicit(&pool->free_head, memory_order_acquire);
        atomic_store_explicit(&header->next, old_head, memory_order_relaxed);
        new_handle = encode_handle(new_gen, user_ptr, ANAM_STATE_FREE);
    } while (!atomic_compare_exchange_weak_explicit(
        &pool->free_head, &old_head, new_handle,
        memory_order_release, memory_order_relaxed));
    
    atomic_fetch_add(&pool->slots_free, 1);
    atomic_fetch_add(&pool->release_count, 1);
    
    return true;
}

/* ============================================================================
 * ACCESS — THE MOMENT OF ANAMNESIS
 * ============================================================================ */

void* anam_get(AnamPool* pool, AnamHandle handle) {
    if (!pool || anam_is_null(handle)) return NULL;
    
    if (anam_state(handle) != ANAM_STATE_LIVE) {
        atomic_fetch_add(&pool->anamnesis_count, 1);
        return NULL;
    }
    
    void* user_ptr = decode_addr(handle);
    if (!in_pool(pool, user_ptr)) {
        atomic_fetch_add(&pool->anamnesis_count, 1);
        return NULL;
    }
    
    SlotHeader* header = get_header(user_ptr);
    
    /* The moment of truth: does the handle's claim match reality? */
    uint16_t claimed_gen = decode_gen(handle);
    uint16_t true_gen = atomic_load(&header->generation);
    
    if (claimed_gen != true_gen) {
        /* Anamnesis: The counterfeit is exposed */
        atomic_fetch_add(&pool->anamnesis_count, 1);
        return NULL;
    }
    
    return user_ptr;
}

bool anam_validate(AnamPool* pool, AnamHandle handle) {
    return anam_get(pool, handle) != NULL;
}

/* ============================================================================
 * DIAGNOSTICS
 * ============================================================================ */

void anam_pool_stats(AnamPool* pool, AnamPoolStats* stats) {
    if (!pool || !stats) return;
    
    stats->slot_count = pool->slot_count;
    stats->slots_free = atomic_load(&pool->slots_free);
    stats->slots_live = pool->slot_count - stats->slots_free;
    stats->alloc_count = atomic_load(&pool->alloc_count);
    stats->release_count = atomic_load(&pool->release_count);
    stats->anamnesis_count = atomic_load(&pool->anamnesis_count);
    stats->generation_max = atomic_load(&pool->generation_max);
}

void anam_foreach(AnamPool* pool, AnamIterFn fn, void* user_data) {
    if (!pool || !fn) return;

    bool* is_free = (bool*)calloc(pool->slot_count, sizeof(bool));
    if (!is_free) return;

    /* Build a snapshot of the free list.
       Not thread-safe; intended only for debugging/inspection. */
    AnamHandle h = atomic_load(&pool->free_head);
    while (!anam_is_null(h)) {
        void* user_ptr = decode_addr(h);
        if (!in_pool(pool, user_ptr)) break;

        size_t idx = (size_t)(((char*)user_ptr - (char*)pool->slots_base) / pool->slot_stride);
        if (idx >= pool->slot_count) break;
        if (is_free[idx]) break; /* detect cycles/corruption */
        is_free[idx] = true;

        SlotHeader* header = get_header(user_ptr);
        h = atomic_load(&header->next);
    }

    for (size_t i = 0; i < pool->slot_count; i++) {
        if (is_free[i]) continue;

        void* user_ptr = slot_from_index(pool, i);
        SlotHeader* header = get_header(user_ptr);
        uint16_t gen = atomic_load(&header->generation);
        AnamHandle handle = encode_handle(gen, user_ptr, ANAM_STATE_LIVE);
        if (!fn(handle, user_ptr, user_data)) break;
    }

    free(is_free);
}
