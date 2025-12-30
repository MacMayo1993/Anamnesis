#!/usr/bin/env python3
"""
Anamnesis Trace Analyzer

Analyzes binary trace files to compute slot reuse entropy and detect
phase transitions in lock-free memory pool allocation patterns.

Hypothesis: There exists a critical contention level k* ≈ 1/(2 ln 2) ≈ 0.721
where the normalized entropy of slot reuse patterns undergoes a phase
transition.

Usage:
    python analyze_traces.py <trace_dir> [options]

Example:
    python analyze_traces.py ./traces --num-slots=1024 --plot
"""

import struct
import numpy as np
from pathlib import Path
from collections import Counter, defaultdict
import argparse
import sys

# Trace entry format (must match anamnesis_trace.h)
TRACE_ENTRY_FORMAT = '<QIHBB'  # timestamp, slot, gen, op_type, thread_id
TRACE_ENTRY_SIZE = struct.calcsize(TRACE_ENTRY_FORMAT)

# Operation types (must match anamnesis_trace.h)
OP_ALLOC = 0
OP_RELEASE = 1
OP_GET_VALID = 2
OP_GET_STALE = 3
OP_VALIDATION_FAIL = 4

OP_NAMES = {
    OP_ALLOC: 'alloc',
    OP_RELEASE: 'release',
    OP_GET_VALID: 'get_valid',
    OP_GET_STALE: 'get_stale',
    OP_VALIDATION_FAIL: 'validate_fail'
}


def load_trace_file(filename):
    """Load binary trace file into list of entries."""
    entries = []
    with open(filename, 'rb') as f:
        while True:
            data = f.read(TRACE_ENTRY_SIZE)
            if not data:
                break
            entry = struct.unpack(TRACE_ENTRY_FORMAT, data)
            entries.append({
                'timestamp': entry[0],
                'slot': entry[1],
                'generation': entry[2],
                'op_type': entry[3],
                'thread_id': entry[4]
            })
    return entries


def merge_traces(trace_dir):
    """Merge all per-thread traces and sort by timestamp."""
    trace_dir = Path(trace_dir)
    all_entries = []

    trace_files = sorted(trace_dir.glob('trace_thread_*.bin'))
    if not trace_files:
        print(f"Error: No trace files found in {trace_dir}", file=sys.stderr)
        return []

    print(f"Loading {len(trace_files)} trace files...")
    for trace_file in trace_files:
        entries = load_trace_file(trace_file)
        all_entries.extend(entries)
        print(f"  {trace_file.name}: {len(entries):,} entries")

    # Sort by timestamp
    print("Sorting entries by timestamp...")
    all_entries.sort(key=lambda e: e['timestamp'])

    return all_entries


def compute_reuse_entropy(entries, num_slots):
    """
    Compute normalized entropy of slot reuse patterns.

    Analyzes the sequence of slot allocations to measure how uniformly
    slots are being reused. High entropy = uniform reuse, low entropy =
    biased reuse (e.g., LIFO stack behavior).

    Returns:
        H_norm: Normalized entropy in [0, 1]
                0 = deterministic (always same slot)
                1 = maximum entropy (uniform distribution)
    """
    # Build sequence of allocated slot indices
    alloc_sequence = []
    for entry in entries:
        if entry['op_type'] == OP_ALLOC:
            alloc_sequence.append(entry['slot'])

    if not alloc_sequence:
        return 0.0

    # Compute empirical probability distribution
    slot_counts = Counter(alloc_sequence)
    probs = np.array(list(slot_counts.values())) / len(alloc_sequence)

    # Shannon entropy
    H = -np.sum(probs * np.log2(probs + 1e-10))

    # Normalize by maximum possible entropy
    H_max = np.log2(num_slots)
    H_norm = H / H_max if H_max > 0 else 0.0

    return H_norm


def analyze_operation_stats(entries):
    """Compute operation statistics."""
    op_counts = Counter(e['op_type'] for e in entries)

    stats = {
        'total_ops': len(entries),
        'allocs': op_counts[OP_ALLOC],
        'releases': op_counts[OP_RELEASE],
        'gets': op_counts[OP_GET_VALID] + op_counts[OP_GET_STALE],
        'stale_gets': op_counts[OP_GET_STALE],
        'validation_fails': op_counts[OP_VALIDATION_FAIL]
    }

    # Compute percentages
    if stats['total_ops'] > 0:
        stats['alloc_pct'] = 100.0 * stats['allocs'] / stats['total_ops']
        stats['release_pct'] = 100.0 * stats['releases'] / stats['total_ops']

    if stats['gets'] > 0:
        stats['stale_rate'] = 100.0 * stats['stale_gets'] / stats['gets']

    return stats


def print_summary(trace_dir, entries, entropy, num_slots):
    """Print analysis summary."""
    stats = analyze_operation_stats(entries)

    print("\n" + "=" * 70)
    print(f"TRACE ANALYSIS: {trace_dir}")
    print("=" * 70)

    print(f"\nOperation Statistics:")
    print(f"  Total operations: {stats['total_ops']:,}")
    print(f"  Allocations:      {stats['allocs']:,} ({stats.get('alloc_pct', 0):.1f}%)")
    print(f"  Releases:         {stats['releases']:,} ({stats.get('release_pct', 0):.1f}%)")
    print(f"  Gets:             {stats['gets']:,}")
    print(f"    ├─ Valid:       {stats['gets'] - stats['stale_gets']:,}")
    print(f"    └─ Stale:       {stats['stale_gets']:,} ({stats.get('stale_rate', 0):.2f}%)")
    print(f"  Validation fails: {stats['validation_fails']:,}")

    print(f"\nSlot Reuse Entropy:")
    print(f"  Normalized entropy: H_norm = {entropy:.4f}")
    print(f"  Max possible:       H_max  = log2({num_slots}) = {np.log2(num_slots):.4f}")

    # Interpret entropy
    print(f"\nInterpretation:")
    if entropy > 0.9:
        print("  → Nearly uniform slot reuse (high contention, random-like)")
    elif entropy > 0.7:
        print("  → Moderately uniform reuse")
    elif entropy > 0.4:
        print("  → Biased reuse pattern")
    else:
        print("  → Highly biased reuse (low contention, LIFO-like)")

    # Check for k* hypothesis
    K_STAR = 1.0 / (2.0 * np.log(2.0))
    if abs(entropy - K_STAR) < 0.05:
        print(f"\n  ⚠️  Entropy near k* = {K_STAR:.4f} — possible phase transition!")


def plot_results(trace_dirs, entropies, contention_levels):
    """Plot entropy vs contention with k* reference line."""
    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("\nWarning: matplotlib not available, skipping plot")
        return

    plt.figure(figsize=(10, 6))
    plt.plot(contention_levels, entropies, 'o-', linewidth=2, markersize=8,
             label='Measured H_norm')

    # k* reference line
    K_STAR = 1.0 / (2.0 * np.log(2.0))
    plt.axhline(K_STAR, color='r', linestyle='--', linewidth=2,
                label=f'k* = 1/(2 ln 2) ≈ {K_STAR:.4f}')

    plt.xlabel('Contention Level (threads)', fontsize=12)
    plt.ylabel('Normalized Slot Reuse Entropy', fontsize=12)
    plt.title('Phase Transition in Lock-Free Allocation Patterns', fontsize=14)
    plt.grid(True, alpha=0.3)
    plt.legend(fontsize=11)
    plt.tight_layout()

    output_pdf = 'entropy_vs_contention.pdf'
    output_png = 'entropy_vs_contention.png'
    plt.savefig(output_pdf, dpi=300)
    plt.savefig(output_png, dpi=300)

    print(f"\n📊 Plots saved:")
    print(f"  {output_pdf}")
    print(f"  {output_png}")


def main():
    parser = argparse.ArgumentParser(description='Analyze Anamnesis trace files')
    parser.add_argument('trace_dir', help='Directory containing trace files')
    parser.add_argument('--num-slots', type=int, default=1024,
                        help='Number of slots in pool (default: 1024)')
    parser.add_argument('--plot', action='store_true',
                        help='Generate entropy vs contention plot')
    parser.add_argument('--multi', action='store_true',
                        help='Analyze multiple trace directories (trace_c1, trace_c2, ...)')

    args = parser.parse_args()

    if args.multi:
        # Analyze multiple contention levels
        base_dir = Path(args.trace_dir)
        results = []

        for contention in [1, 2, 4, 8, 16, 32, 64]:
            trace_dir = base_dir / f'traces_c{contention}'
            if not trace_dir.exists():
                print(f"Skipping {trace_dir} (not found)")
                continue

            print(f"\nProcessing contention level {contention}...")
            entries = merge_traces(trace_dir)
            if not entries:
                continue

            entropy = compute_reuse_entropy(entries, args.num_slots)
            print_summary(trace_dir, entries, entropy, args.num_slots)

            results.append({
                'contention': contention,
                'entropy': entropy,
                'dir': trace_dir
            })

        if results and args.plot:
            contentions = [r['contention'] for r in results]
            entropies = [r['entropy'] for r in results]
            trace_dirs = [r['dir'] for r in results]
            plot_results(trace_dirs, entropies, contentions)

    else:
        # Single trace directory
        entries = merge_traces(args.trace_dir)
        if not entries:
            return 1

        entropy = compute_reuse_entropy(entries, args.num_slots)
        print_summary(args.trace_dir, entries, entropy, args.num_slots)

    return 0


if __name__ == '__main__':
    sys.exit(main())
