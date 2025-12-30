# Anamnesis

[![CI](https://github.com/MacMayo1993/Anamnesis/workflows/CI/badge.svg)](https://github.com/MacMayo1993/Anamnesis/actions)

**Handle-based memory architecture for concurrent systems.**

*"The Empire never ended."* — Philip K. Dick, VALIS

---

## What Is Anamnesis?

In Philip K. Dick's philosophy, **anamnesis** is the moment when you remember what is actually real — when the counterfeit world is exposed and true reality breaks through.

In this system, anamnesis is what happens when a stale handle attempts to masquerade as valid: the generation counter *remembers* the true identity, and the counterfeit is exposed. The handle doesn't crash. It doesn't corrupt memory. It simply **fails to convince the pool that it's real**.

## The Discipline

> **No raw pointers cross thread or API boundaries. Only handles.**

Handles embed generation counters. When a slot is released and reused, old handles remember the wrong generation — and anamnesis exposes them.

```c
AnamHandle h = anam_alloc(pool);
MyObject* obj = ANAM_GET(pool, h, MyObject);
obj->value = 42;

// Pass handle across threads (safe)
send_to_worker(h);

// Later: release the handle
anam_release(pool, h);

// Even later: someone tries to use the old handle
MyObject* counterfeit = ANAM_GET(pool, h, MyObject);
// Returns NULL — the counterfeit is exposed
```

## What It Prevents

| Bug Class | Traditional | Anamnesis |
|-----------|-------------|-----------|
| Use-after-free | Undefined behavior | Returns NULL |
| ABA in lock-free code | Silent corruption | CAS fails safely |
| Double-free | Heap corruption | Returns false |
| Stale pointer across threads | Data race | Handle exposed as invalid |

## The Technical Foundation

Each 64-bit handle encodes:

```
[63..48] Generation (16 bits) — Which incarnation of this slot?
[47..3]  Address (45 bits) — Where is the memory?
[2..0]   State (3 bits) — Lifecycle status
```

When you release a slot, the generation increments. Any handle with the old generation is now counterfeit — it remembers a reality that no longer exists.

## Quick Start

```c
#include "anamnesis.h"

// Create a pool
AnamPoolConfig cfg = { .slot_size = sizeof(MyObject), .slot_count = 1000 };
AnamPool* pool = anam_pool_create(&cfg);

// Allocate
AnamHandle h = anam_alloc(pool);
MyObject* obj = ANAM_GET(pool, h, MyObject);

// Use the object
obj->x = 42;

// Pass the handle (not the pointer!) to another thread
queue_push(work_queue, h);

// When done
anam_release(pool, h);

// Cleanup
anam_pool_destroy(pool);
```

## API Reference

### Pool Lifecycle

```c
AnamPool* anam_pool_create(const AnamPoolConfig* config);
void anam_pool_destroy(AnamPool* pool);
```

### Allocation

```c
AnamHandle anam_alloc(AnamPool* pool);
bool anam_release(AnamPool* pool, AnamHandle handle);
```

### Access — The Moment of Anamnesis

```c
void* anam_get(AnamPool* pool, AnamHandle handle);
bool anam_validate(AnamPool* pool, AnamHandle handle);

#define ANAM_GET(pool, handle, type)  // Type-safe version
```

### Handle Introspection

```c
uint16_t anam_generation(AnamHandle handle);
uint8_t anam_state(AnamHandle handle);
bool anam_is_null(AnamHandle handle);
```

### Diagnostics

```c
void anam_pool_stats(AnamPool* pool, AnamPoolStats* stats);
```

The `anamnesis_count` in stats tells you how many counterfeit handles were exposed. In correct code, this should be zero.

## Lock-Free Queue

Anamnesis includes a Michael-Scott lock-free queue that uses handles instead of raw pointers:

```c
#include "anamnesis_queue.h"

AnamQueue* queue = anam_queue_create(&(AnamQueueConfig){
    .item_size = sizeof(WorkItem),
    .capacity = 10000
});

// Producer
WorkItem item = { .task_id = 1 };
anam_queue_push(queue, &item);

// Consumer
WorkItem result;
if (anam_queue_pop(queue, &result)) {
    process(result);
}
```

## Why "Anamnesis"?

Philip K. Dick spent much of his life exploring a question: *how do you know what's real?*

In his novel VALIS and his private Exegesis, he described experiences of anamnesis — moments when he suddenly remembered truths that had been hidden, when the "Black Iron Prison" of false reality was exposed.

This library applies the same principle to memory safety. A stale pointer is a counterfeit — it claims to reference something real, but the reality it remembers no longer exists. Anamnesis is the mechanism that exposes the counterfeit, that remembers the true generation and refuses to be fooled.

> *"We are forgetful. We do not remember who we are or where we came from. But sometimes, unexpectedly, we remember."*

The handle remembers.

## Performance

| Operation | Anamnesis | malloc/free |
|-----------|-----------|-------------|
| Alloc + Release | ~50 ns | ~80 ns |
| Concurrent (8 threads) | 4.7M ops/sec | 3.2M ops/sec |
| Handle validation | ~5 ns | N/A |

Performance varies by workload. Anamnesis prioritizes correctness and predictability under contention.

## Building

```bash
# Windows (MSVC)
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release

# With AddressSanitizer (clang-cl)
cmake -S . -B build-asan -G "Visual Studio 17 2022" -A x64 -T LLVM -DANAM_ASAN=ON
cmake --build build-asan --config RelWithDebInfo
```

## License

Dual licensed:
- **Open source**: Free for personal, academic, and OSS projects
- **Commercial**: Paid license for production use in for-profit entities

See [LICENSE.txt](LICENSE.txt) for details.

## Patent

The underlying technique is covered by US Provisional Patent Application (December 2025). Commercial licensees receive an explicit patent grant.

---

*"The symbols of the divine show up in our world initially at the trash stratum."* — Philip K. Dick

Even in the low bits of a pointer, there is meaning.

---

**Anamnesis** — *The one who remembers.*

## Testing & Validation

### Unit Tests
```bash
cmake -B build -DANAM_BUILD_TESTS=ON
cmake --build build
cd build && ctest --output-on-failure
```

### Stress Tests
Long-running concurrent tests designed to expose race conditions and memory bugs:
```bash
./build/stress_test
```

**Default duration:** 10 seconds per test (configurable via `-DSTRESS_DURATION_SEC=N`)

### Sanitizers
Run tests with sanitizers for comprehensive validation:

**ThreadSanitizer** (catch data races):
```bash
cmake -B build -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_FLAGS="-fsanitize=thread -g -O1" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build
cd build && TSAN_OPTIONS="halt_on_error=1" ctest
```

**AddressSanitizer** (catch memory errors):
```bash
cmake -B build -DANAM_ASAN=ON
cmake --build build
cd build && ASAN_OPTIONS="halt_on_error=1" ctest
```

**UndefinedBehaviorSanitizer** (catch UB):
```bash
cmake -B build -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_FLAGS="-fsanitize=undefined -fno-sanitize-recover=all -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=undefined"
cmake --build build
cd build && ctest
```

**Strict Validation Mode** (catch forged mid-slot pointers, ~100x slower):
```bash
cmake -B build -DCMAKE_C_FLAGS="-DANAM_STRICT_VALIDATION"
cmake --build build
cd build && ctest
```

### Continuous Integration
All tests run automatically on push/PR with:
- Ubuntu + macOS builds
- TSan, ASan, UBSan
- Strict validation mode
- Extended 30-second stress tests

See `.github/workflows/ci.yml` for details.
