/**
 * Anamnesis Queue - Implementation
 * 
 * Michael-Scott lock-free queue with handle-based ABA prevention.
 */

#include "anamnesis_queue.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#define ANAM_ADDR_MASK  0x0000FFFFFFFFFFF8ULL

static inline AnamHandle encode(uint16_t gen, void* addr, uint8_t state) {
    return ((uint64_t)gen << 48) | ((uint64_t)(uintptr_t)addr & ANAM_ADDR_MASK) | (state & 0x7);
}

static inline void* decode_addr(AnamHandle h) {
    return (void*)(uintptr_t)(h & ANAM_ADDR_MASK);
}

static inline uint16_t decode_gen(AnamHandle h) {
    return (uint16_t)(h >> 48);
}

typedef struct QueueNode {
    _Atomic(AnamHandle) next;
} QueueNode;

struct AnamQueue {
    size_t item_size;
    size_t capacity;
    size_t node_size;
    AnamPool* pool;
    _Atomic(AnamHandle) head;
    _Atomic(AnamHandle) tail;
    _Atomic(size_t) length;
    _Atomic(size_t) push_count;
    _Atomic(size_t) pop_count;
    _Atomic(size_t) push_fails;
    _Atomic(size_t) pop_fails;
    _Atomic(size_t) aba_prevented;
};

static inline void* node_data(QueueNode* node) {
    return (void*)((char*)node + sizeof(QueueNode));
}

static AnamHandle alloc_node(AnamQueue* q, const void* data) {
    AnamHandle h = anam_alloc(q->pool);
    if (anam_is_null(h)) return ANAM_NULL;
    
    QueueNode* node = (QueueNode*)anam_get(q->pool, h);
    atomic_store(&node->next, ANAM_NULL);
    if (data) memcpy(node_data(node), data, q->item_size);
    
    return h;
}

AnamQueue* anam_queue_create(const AnamQueueConfig* config) {
    AnamQueueConfig cfg = ANAM_QUEUE_DEFAULT;
    if (config) cfg = *config;
    
    if (cfg.item_size == 0 || cfg.capacity == 0) return NULL;
    
    AnamQueue* q = (AnamQueue*)calloc(1, sizeof(AnamQueue));
    if (!q) return NULL;
    
    q->item_size = cfg.item_size;
    q->capacity = cfg.capacity;
    q->node_size = ((sizeof(QueueNode) + cfg.item_size + 7) & ~7);
    
    AnamPoolConfig pool_cfg = {
        .slot_size = q->node_size,
        .slot_count = cfg.capacity + 1
    };
    q->pool = anam_pool_create(&pool_cfg);
    if (!q->pool) { free(q); return NULL; }
    
    AnamHandle dummy = alloc_node(q, NULL);
    if (anam_is_null(dummy)) {
        anam_pool_destroy(q->pool);
        free(q);
        return NULL;
    }
    
    atomic_store(&q->head, dummy);
    atomic_store(&q->tail, dummy);
    
    return q;
}

void anam_queue_destroy(AnamQueue* q) {
    if (!q) return;
    while (anam_queue_pop(q, NULL)) {}
    AnamHandle head = atomic_load(&q->head);
    if (!anam_is_null(head)) anam_release(q->pool, head);
    if (q->pool) anam_pool_destroy(q->pool);
    free(q);
}

AnamHandle anam_queue_push(AnamQueue* q, const void* data) {
    if (!q || !data) return ANAM_NULL;
    
    AnamHandle new_h = alloc_node(q, data);
    if (anam_is_null(new_h)) {
        atomic_fetch_add(&q->push_fails, 1);
        return ANAM_NULL;
    }
    
    while (1) {
        AnamHandle tail_h = atomic_load_explicit(&q->tail, memory_order_acquire);
        QueueNode* tail = (QueueNode*)anam_get(q->pool, tail_h);
        if (!tail) { atomic_fetch_add(&q->aba_prevented, 1); continue; }
        
        AnamHandle next_h = atomic_load_explicit(&tail->next, memory_order_acquire);
        if (tail_h != atomic_load(&q->tail)) { atomic_fetch_add(&q->aba_prevented, 1); continue; }
        
        if (anam_is_null(next_h)) {
            if (atomic_compare_exchange_weak_explicit(&tail->next, &next_h, new_h,
                    memory_order_release, memory_order_relaxed)) break;
        } else {
            uint16_t gen = decode_gen(next_h);
            void* addr = decode_addr(next_h);
            atomic_compare_exchange_weak(&q->tail, &tail_h, encode(gen, addr, ANAM_STATE_LIVE));
        }
    }
    
    AnamHandle tail_h = atomic_load(&q->tail);
    uint16_t gen = decode_gen(new_h);
    void* addr = decode_addr(new_h);
    atomic_compare_exchange_strong(&q->tail, &tail_h, encode(gen, addr, ANAM_STATE_LIVE));
    
    atomic_fetch_add(&q->length, 1);
    atomic_fetch_add(&q->push_count, 1);
    return new_h;
}

bool anam_queue_pop(AnamQueue* q, void* data_out) {
    if (!q) return false;
    
    while (1) {
        AnamHandle head_h = atomic_load_explicit(&q->head, memory_order_acquire);
        AnamHandle tail_h = atomic_load_explicit(&q->tail, memory_order_acquire);
        QueueNode* head = (QueueNode*)anam_get(q->pool, head_h);
        if (!head) { atomic_fetch_add(&q->aba_prevented, 1); continue; }
        
        AnamHandle next_h = atomic_load_explicit(&head->next, memory_order_acquire);
        if (head_h != atomic_load(&q->head)) { atomic_fetch_add(&q->aba_prevented, 1); continue; }
        
        if (head_h == tail_h) {
            if (anam_is_null(next_h)) {
                atomic_fetch_add(&q->pop_fails, 1);
                return false;
            }
            uint16_t gen = decode_gen(next_h);
            void* addr = decode_addr(next_h);
            atomic_compare_exchange_weak(&q->tail, &tail_h, encode(gen, addr, ANAM_STATE_LIVE));
        } else {
            QueueNode* next = (QueueNode*)anam_get(q->pool, next_h);
            if (!next) { atomic_fetch_add(&q->aba_prevented, 1); continue; }
            
            if (data_out) memcpy(data_out, node_data(next), q->item_size);
            
            uint16_t gen = decode_gen(next_h);
            void* addr = decode_addr(next_h);
            if (atomic_compare_exchange_weak_explicit(&q->head, &head_h, 
                    encode(gen, addr, ANAM_STATE_LIVE), memory_order_release, memory_order_relaxed)) {
                anam_release(q->pool, head_h);
                atomic_fetch_sub(&q->length, 1);
                atomic_fetch_add(&q->pop_count, 1);
                return true;
            }
        }
    }
}

bool anam_queue_peek(AnamQueue* q, void* data_out) {
    if (!q || !data_out) return false;
    AnamHandle head_h = atomic_load(&q->head);
    QueueNode* head = (QueueNode*)anam_get(q->pool, head_h);
    if (!head) return false;
    AnamHandle next_h = atomic_load(&head->next);
    if (anam_is_null(next_h)) return false;
    QueueNode* next = (QueueNode*)anam_get(q->pool, next_h);
    if (!next) return false;
    memcpy(data_out, node_data(next), q->item_size);
    return true;
}

bool anam_queue_empty(AnamQueue* q) {
    if (!q) return true;
    AnamHandle head_h = atomic_load(&q->head);
    QueueNode* head = (QueueNode*)anam_get(q->pool, head_h);
    if (!head) return true;
    return anam_is_null(atomic_load(&head->next));
}

size_t anam_queue_length(AnamQueue* q) {
    return q ? atomic_load(&q->length) : 0;
}

void anam_queue_stats(AnamQueue* q, AnamQueueStats* stats) {
    if (!q || !stats) return;
    stats->capacity = q->capacity;
    stats->push_count = atomic_load(&q->push_count);
    stats->pop_count = atomic_load(&q->pop_count);
    stats->push_fails = atomic_load(&q->push_fails);
    stats->pop_fails = atomic_load(&q->pop_fails);
    stats->aba_prevented = atomic_load(&q->aba_prevented);
}
