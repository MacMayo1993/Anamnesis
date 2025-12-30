#!/bin/bash
for c in 1 2 3 4 6 8 12; do
    echo -n "$c "
    python3 tools/analyze_traces.py traces_c${c} --num-slots=1024 2>/dev/null | grep "Normalized entropy:" | awk '{print $5}'
done
