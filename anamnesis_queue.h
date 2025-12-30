/**
 * Anamnesis Queue - Lock-Free MPMC Queue
 * 
 * Michael-Scott queue implementation using Anamnesis handles.
 * 
 * In traditional lock-free queues, the ABA problem lurks: a node is
 * dequeued, freed, reallocated to the same address, and re-enqueued.
 * A pending CAS succeeds because the address matches — but the node
 * is not the same. Silent corruption follows.
 * 
 * With Anamnesis, every node carries a generation. When the slot is
 * reused, the generation increments. The pending CAS fails because
 * the handle remembers the old generation — it cannot be fooled by
 * a counterfeit occupying the same address.
 * 
 * The queue doesn't just prevent ABA. It *exposes* it.
 */

#ifndef ANAMNESIS_QUEUE_H
#define ANAMNESIS_QUEUE_H

#include "anamnesis.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * QUEUE TYPE
 * ============================================================================ */

typedef struct AnamQueue AnamQueue;

/* ============================================================================
 * CONFIGURATION
 * ============================================================================ */

typedef struct AnamQueueConfig {
    size_t item_size;   /* Size of each queued item */
    size_t capacity;    /* Maximum items */
} AnamQueueConfig;

#define ANAM_QUEUE_DEFAULT { .item_size = 64, .capacity = 1024 }

/* ============================================================================
 * LIFECYCLE
 * ============================================================================ */

AnamQueue* anam_queue_create(const AnamQueueConfig* config);
void anam_queue_destroy(AnamQueue* queue);

/* ============================================================================
 * OPERATIONS
 * ============================================================================ */

/**
 * Enqueue an item.
 * 
 * @return Handle to enqueued node, or ANAM_NULL if queue full
 */
AnamHandle anam_queue_push(AnamQueue* queue, const void* data);

/**
 * Dequeue an item.
 * 
 * @return true if item dequeued, false if queue empty
 */
bool anam_queue_pop(AnamQueue* queue, void* data_out);

/**
 * Peek at front item without removing.
 */
bool anam_queue_peek(AnamQueue* queue, void* data_out);

/* ============================================================================
 * QUERY
 * ============================================================================ */

bool anam_queue_empty(AnamQueue* queue);
size_t anam_queue_length(AnamQueue* queue);

/* ============================================================================
 * STATISTICS
 * ============================================================================ */

typedef struct AnamQueueStats {
    size_t capacity;
    size_t push_count;
    size_t pop_count;
    size_t push_fails;      /* Queue was full */
    size_t pop_fails;       /* Queue was empty */
    size_t aba_prevented;   /* CAS retries due to generation mismatch */
} AnamQueueStats;

void anam_queue_stats(AnamQueue* queue, AnamQueueStats* stats);

#ifdef __cplusplus
}
#endif

/* ============================================================================
 * C++ WRAPPER
 * ============================================================================ */

#ifdef __cplusplus
#include <optional>

namespace anam {

template<typename T>
class Queue {
    AnamQueue* queue_;
    
public:
    explicit Queue(size_t capacity = 1024) {
        AnamQueueConfig cfg = { sizeof(T), capacity };
        queue_ = anam_queue_create(&cfg);
    }
    
    ~Queue() { if (queue_) anam_queue_destroy(queue_); }
    
    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;
    
    bool push(const T& item) {
        return !anam_is_null(anam_queue_push(queue_, &item));
    }
    
    std::optional<T> pop() {
        T item;
        if (anam_queue_pop(queue_, &item)) return item;
        return std::nullopt;
    }
    
    std::optional<T> peek() const {
        T item;
        if (anam_queue_peek(queue_, &item)) return item;
        return std::nullopt;
    }
    
    bool empty() const { return anam_queue_empty(queue_); }
    size_t length() const { return anam_queue_length(queue_); }
};

} // namespace anam
#endif

#endif /* ANAMNESIS_QUEUE_H */
