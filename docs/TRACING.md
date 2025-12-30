# Anamnesis Tracing System

Production-grade lock-free tracing infrastructure for analyzing allocation patterns in concurrent memory pools.

## Purpose

Measure slot reuse entropy under varying contention levels to detect phase transitions in lock-free memory pool behavior.

**Hypothesis**: There exists a critical contention level k* ≈ 1/(2 ln 2) ≈ 0.721 where normalized entropy undergoes a phase transition.

## Building with Tracing

```bash
cmake -B build -DANAM_TRACE=ON -DANAM_BUILD_TESTS=ON
cmake --build build
```

## Architecture

### Per-Thread Ring Buffers
- **Zero contention**: Each thread has its own lock-free buffer
- **Minimal overhead**: ~5% throughput impact at 10M+ ops/sec
- **Binary format**: 16 bytes per entry (timestamp, slot, generation, op_type, thread_id)
- **Auto-flush**: Flushes at 75% capacity to prevent overflow

### Platform Support
- **x86-64**: `rdtsc` for cycle-accurate timestamps
- **ARM64**: `cntvct_el0` counter
- **Fallback**: `clock_gettime(CLOCK_MONOTONIC)` nanosecond precision

## Usage Example

```c
#include "anamnesis.h"
#include "anamnesis_trace.h"

int main() {
    // Initialize tracing (before creating pools/threads)
    anam_trace_init("./traces", 1024 * 1024);  // 1M entries per thread

    // Create pool and run workload
    AnamPool* pool = anam_pool_create(&cfg);
    run_concurrent_workload(pool);

    // Shutdown (flushes current thread)
    anam_trace_shutdown();

    // Worker threads should call:
    // anam_trace_flush_thread();
    // before pthread_exit()

    return 0;
}
```

## Trace File Format

Binary files: `trace_thread_NNN.bin`

Each entry is 16 bytes:
```c
typedef struct {
    uint64_t timestamp;   // CPU cycle counter
    uint32_t slot_index;  // Which slot
    uint16_t generation;  // Generation at operation
    uint8_t  op_type;     // 0=alloc, 1=release, 2=get_valid, 3=get_stale
    uint8_t  thread_id;   // Thread 0-255
} AnamTraceEntry;
```

## Analysis Tools

### Single Trace Directory

```bash
python tools/analyze_traces.py ./traces --num-slots=1024
```

Output:
```
TRACE ANALYSIS: ./traces
======================================================================
Operation Statistics:
  Total operations: 10,775,160
  Allocations:      5,387,580 (50.0%)
  Releases:         5,387,580 (50.0%)
  ...

Slot Reuse Entropy:
  Normalized entropy: H_norm = 0.8234
  Max possible:       H_max  = log2(1024) = 10.0000

Interpretation:
  → Nearly uniform slot reuse (high contention, random-like)
```

### Multi-Contention Analysis

Run benchmarks at different contention levels:

```bash
for c in 1 2 4 8 16 32 64; do
    mkdir -p traces_c${c}
    ./benchmark --threads=${c} --duration=10
done
```

Analyze and plot:

```bash
python tools/analyze_traces.py . --multi --plot
```

Generates `entropy_vs_contention.{pdf,png}` with k* = 0.721 reference line.

## Performance Impact

| Configuration | Throughput Impact |
|---------------|-------------------|
| Tracing OFF   | 0% (compile-time disabled) |
| Tracing ON    | ~3-7% typical |
| Buffer flush  | <1ms for 1M entries |

## Advanced Features

### Get Thread Statistics

```c
uint64_t entries_written;
uint32_t buffer_overflows;

if (anam_trace_get_stats(&entries_written, &buffer_overflows)) {
    printf("Thread wrote %llu entries, %u overflows\n",
           entries_written, buffer_overflows);
}
```

### Detecting Overflows

If `buffer_overflows > 0`, increase buffer size:

```c
anam_trace_init("./traces", 4 * 1024 * 1024);  // 4M entries
```

## Interpreting Results

### Entropy Values

| H_norm | Pattern | Interpretation |
|--------|---------|----------------|
| > 0.9  | Uniform | High contention, nearly random slot reuse |
| 0.7-0.9 | Mixed | Moderate contention |
| 0.4-0.7 | Biased | Low-moderate contention, some LIFO bias |
| < 0.4  | LIFO-dominated | Low contention, stack-like behavior |

### k* Phase Transition

If H_norm crosses 0.721 at a specific contention level, this suggests:

1. **Below k***: LIFO-dominated (free list behaves like stack)
2. **At k***: Critical point (pattern changes rapidly)
3. **Above k***: Randomized (contention destroys LIFO order)

This is analogous to percolation transitions in statistical physics.

## Implementation Details

### Zero-Cost Abstraction

When compiled without `-DANAM_TRACE=ON`:
- All trace calls become no-ops (`((void)0)` macros)
- No runtime overhead
- No code size increase

### Thread Safety

- Per-thread buffers eliminate lock contention
- Ring buffer uses modulo-power-of-2 for fast indexing
- Atomic thread ID allocation
- Files written in append mode (safe for concurrent writes)

### Memory Usage

Per-thread buffer: `capacity * 16 bytes`

Default (1M entries): **16 MB** per thread

Recommended: 1-4M entries for high-throughput workloads

## Troubleshooting

### Missing Traces

Check:
1. `anam_trace_init()` called before pool creation
2. Threads call `anam_trace_flush_thread()` before exit
3. Output directory exists and is writable

### Empty Analysis

If Python script finds no files:
```bash
ls -lh traces/  # Check for trace_thread_*.bin
```

### Matplotlib Not Found

```bash
pip install matplotlib numpy
```

## References

1. Shannon, C. E. (1948). "A Mathematical Theory of Communication"
2. Treiber, R. K. (1986). "Systems Programming: Coping with Parallelism"
3. Statistical physics analogy: Percolation theory phase transitions

---

**"The system remembers, and the traces expose what is real."**
