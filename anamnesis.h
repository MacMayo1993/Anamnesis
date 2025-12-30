/**
 * Anamnesis - Handle-Based Memory Architecture
 * 
 * "The one who remembers."
 * 
 * Named for Philip K. Dick's concept of anamnesis — the sudden recognition
 * of true reality breaking through the counterfeit. In VALIS and his
 * Exegesis, Dick described anamnesis as the moment when you remember
 * what is actually real, piercing through the "Black Iron Prison" of
 * false perception.
 * 
 * In this system, anamnesis is what happens when a stale handle attempts
 * to masquerade as valid: the generation counter *remembers* the true
 * identity, and the counterfeit is exposed. The handle doesn't just fail —
 * it reveals that what you thought was real never was.
 * 
 * "The Empire never ended." Neither did the stale pointer. Anamnesis
 * just lets you see it.
 * 
 * Technical: Dual-end pointer tagging with generation counters for
 * ABA prevention and use-after-free detection in concurrent systems.
 * 
 * License: Dual (Open Source / Commercial)
 * Patent: US Provisional Application December 2025
 * 
 * Copyright (c) 2025 Mac Mayo. All rights reserved.
 */

#ifndef ANAMNESIS_H
#define ANAMNESIS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * THE HANDLE
 * 
 * AnamHandle is the fundamental type. It is the ONLY type that should
 * cross thread or API boundaries. Never raw pointers. Only handles.
 * 
 * The handle remembers:
 *   - Generation (high 16 bits): Which incarnation of this slot?
 *   - Address (middle 45 bits): Where is the actual memory?
 *   - State (low 3 bits): What is the lifecycle status?
 * 
 * When you release a slot and it's reused, the generation increments.
 * Old handles still point to the same address, but they remember the
 * wrong generation — and anamnesis exposes the counterfeit.
 * ============================================================================ */

typedef uint64_t AnamHandle;

#define ANAM_NULL ((AnamHandle)0)

/* State bits */
#define ANAM_STATE_FREE       0x0  /* Slot available for allocation */
#define ANAM_STATE_LIVE       0x1  /* Slot currently in use */
#define ANAM_STATE_QUARANTINE 0x2  /* Slot pending reclamation */
#define ANAM_STATE_LOCKED     0x4  /* CAS lock bit */

/* ============================================================================
 * POOL CONFIGURATION
 * ============================================================================ */

typedef struct AnamPoolConfig {
    size_t slot_size;       /* Size of each object (aligned to 8) */
    size_t slot_count;      /* Number of slots (fixed at creation) */
    size_t alignment;       /* Minimum alignment (default: 8) */
    bool   zero_on_alloc;   /* Zero memory on allocation */
    bool   zero_on_release; /* Zero memory on release */
} AnamPoolConfig;

#define ANAM_POOL_DEFAULT { \
    .slot_size = 64,        \
    .slot_count = 1024,     \
    .alignment = 8,         \
    .zero_on_alloc = false, \
    .zero_on_release = false \
}

/* ============================================================================
 * POOL (opaque)
 * ============================================================================ */

typedef struct AnamPool AnamPool;

/* ============================================================================
 * LIFECYCLE
 * ============================================================================ */

/**
 * Create a new pool.
 * 
 * The pool is the keeper of memory — it remembers the true generation
 * of every slot, and exposes any handle that claims a false identity.
 */
AnamPool* anam_pool_create(const AnamPoolConfig* config);

/**
 * Destroy a pool.
 * 
 * All handles become invalid. The memory forgets.
 */
void anam_pool_destroy(AnamPool* pool);

/* ============================================================================
 * ALLOCATION
 * ============================================================================ */

/**
 * Allocate a slot and receive a handle.
 * 
 * The handle encodes the current generation. If you hold this handle
 * after the slot is released and reused, anamnesis will expose it.
 * 
 * @return Handle to allocated slot, or ANAM_NULL if pool exhausted
 */
AnamHandle anam_alloc(AnamPool* pool);

/**
 * Release a slot.
 * 
 * The generation increments. Any handle with the old generation
 * is now counterfeit — it remembers a reality that no longer exists.
 * 
 * @return true if released, false if handle was already invalid
 */
bool anam_release(AnamPool* pool, AnamHandle handle);

/* ============================================================================
 * ACCESS — THE MOMENT OF ANAMNESIS
 * ============================================================================ */

/**
 * Attempt to access the memory behind a handle.
 * 
 * This is the moment of truth. The handle claims an identity; the pool
 * remembers the actual generation. If they match, you receive a pointer.
 * If they don't — if the handle is counterfeit — you receive NULL.
 * 
 * The handle doesn't crash. It doesn't corrupt. It simply *fails to
 * convince* the pool that it's real.
 * 
 * @return Pointer to object, or NULL if the handle is stale/invalid
 */
void* anam_get(AnamPool* pool, AnamHandle handle);

/**
 * Type-safe access macro.
 * 
 * Usage: MyStruct* obj = ANAM_GET(pool, handle, MyStruct);
 */
#define ANAM_GET(pool, handle, type) ((type*)anam_get((pool), (handle)))

/**
 * Check if a handle is currently valid.
 * 
 * This is anamnesis as a question rather than an action: "Is this real?"
 */
bool anam_validate(AnamPool* pool, AnamHandle handle);

/* ============================================================================
 * HANDLE INTROSPECTION
 * ============================================================================ */

/**
 * Extract the generation from a handle.
 * 
 * This is what the handle *claims* to be. Whether the pool agrees
 * is another matter.
 */
static inline uint16_t anam_generation(AnamHandle handle) {
    return (uint16_t)(handle >> 48);
}

/**
 * Extract the state bits from a handle.
 */
static inline uint8_t anam_state(AnamHandle handle) {
    return (uint8_t)(handle & 0x7);
}

/**
 * Check if handle is null.
 */
static inline bool anam_is_null(AnamHandle handle) {
    return handle == ANAM_NULL;
}

/* ============================================================================
 * DIAGNOSTICS — OBSERVING THE SYSTEM
 * ============================================================================ */

typedef struct AnamPoolStats {
    size_t slot_count;       /* Total slots */
    size_t slots_free;       /* Currently free */
    size_t slots_live;       /* Currently allocated */
    size_t alloc_count;      /* Total allocations ever */
    size_t release_count;    /* Total releases ever */
    size_t anamnesis_count;  /* Failed validations (counterfeit handles exposed) */
    uint16_t generation_max; /* Highest generation observed */
} AnamPoolStats;

/**
 * Get pool statistics.
 * 
 * The anamnesis_count tells you how many times a counterfeit handle
 * was exposed. In a correct program, this should be zero. If it's
 * nonzero, something is holding stale handles.
 */
void anam_pool_stats(AnamPool* pool, AnamPoolStats* stats);

/* ============================================================================
 * ITERATION (debugging only)
 * ============================================================================ */

typedef bool (*AnamIterFn)(AnamHandle handle, void* ptr, void* user_data);

/**
 * Iterate over all live slots.
 * 
 * Not thread-safe. For debugging only.
 */
void anam_foreach(AnamPool* pool, AnamIterFn fn, void* user_data);

#ifdef __cplusplus
}
#endif

/* ============================================================================
 * C++ WRAPPER
 * ============================================================================ */

#ifdef __cplusplus
#include <type_traits>
#include <optional>

namespace anam {

/**
 * Typed handle — remembers its type at compile time.
 */
template<typename T>
class Handle {
    AnamHandle handle_;
public:
    Handle() : handle_(ANAM_NULL) {}
    explicit Handle(AnamHandle h) : handle_(h) {}
    
    AnamHandle raw() const { return handle_; }
    bool is_null() const { return anam_is_null(handle_); }
    uint16_t generation() const { return anam_generation(handle_); }
    
    explicit operator bool() const { return !is_null(); }
};

/**
 * Pool wrapper with type-safe allocation.
 */
template<typename T>
class Pool {
    AnamPool* pool_;
    
public:
    explicit Pool(size_t count = 1024) {
        AnamPoolConfig cfg = {
            sizeof(T), count, alignof(T), false, false
        };
        pool_ = anam_pool_create(&cfg);
    }
    
    ~Pool() { if (pool_) anam_pool_destroy(pool_); }
    
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;
    
    Handle<T> alloc() { return Handle<T>(anam_alloc(pool_)); }
    bool release(Handle<T> h) { return anam_release(pool_, h.raw()); }
    
    T* get(Handle<T> h) { return static_cast<T*>(anam_get(pool_, h.raw())); }
    bool validate(Handle<T> h) { return anam_validate(pool_, h.raw()); }
    
    AnamPoolStats stats() const {
        AnamPoolStats s;
        anam_pool_stats(pool_, &s);
        return s;
    }
};

} // namespace anam
#endif

#endif /* ANAMNESIS_H */
