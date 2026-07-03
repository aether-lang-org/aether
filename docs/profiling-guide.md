# Aether Runtime Profiling Guide

## Quick Start

### 1. Compile with Profiling Enabled

```bash
gcc -O2 -march=native -DAETHER_PROFILE your_app.c \
    runtime/utils/aether_runtime_profile.c \
    -Iruntime -o your_app_profiled
```

### 2. Run and Get Reports

```bash
./your_app_profiled
```

Output includes:
- Per-core cycle counts for all operations
- Average cycles per operation
- Throughput estimates derived from cycle counts and the host's clock speed
- CSV export for trend analysis

## Example Output

The sample below shows the structure of a run. Real numbers depend on CPU, clock speed, build flags, and workload; always run the profiled build on your own target hardware to get meaningful figures.

```
--- Core 0 ---
  Mailbox Send:         <N> ops,    <avg> cycles/op
  Mailbox Receive:      <N> ops,    <avg> cycles/op
  Batch Send:           <N> msgs,   <avg> cycles/msg
  SPSC Enqueue:         <N> ops,    <avg> cycles/op
  Actor Step:           <N> ops,    <avg> cycles/op

=== Performance Summary ===
Total Messages:   <N>
Avg Cycles/Msg:   <avg>
Throughput:       <N> M msg/sec  (= clock_GHz × 1000 / Avg_Cycles/Msg)
```

## What Gets Measured

### Mailbox Operations
- `mailbox_send()` - Single message send
- `mailbox_receive()` - Single message receive
- `mailbox_send_batch()` - Batched sends
- `mailbox_receive_batch()` - Batched receives

### SPSC Queue (Same-Core Fast Path)
- `spsc_enqueue()` - Lock-free enqueue
- `spsc_dequeue()` - Lock-free dequeue

### Cross-Core Messaging
- `queue_enqueue()` - Cross-core message queue
- `queue_dequeue()` - Dequeue from shared queue

### Scheduler
- `actor_step()` - Actor processing time
- Idle cycles
- Atomic operation counts
- Lock contention tracking

## Production Use

### Zero Overhead in Production

Without `-DAETHER_PROFILE`, all profiling macros compile to `((void)0)` - **zero overhead**.

```bash
# Production build - no profiling
gcc -O2 -march=native your_app.c -o your_app_prod

# Development build - with profiling
gcc -O2 -march=native -DAETHER_PROFILE your_app.c \
    runtime/utils/aether_runtime_profile.c -o your_app_dev
```

### Continuous Integration

Use profiled builds in CI to detect performance regressions:

```bash
# Run profiled benchmark
./bench_profiled.exe

# Export to CSV
# Result: profile_results.csv

# Compare the CSV output against a saved baseline manually,
# or integrate with your preferred benchmarking toolchain.
```

## CSV Export Format

```csv
core,operation,count,total_cycles,avg_cycles
0,mailbox_send,<N>,<total>,<avg>
0,mailbox_receive,<N>,<total>,<avg>
0,batch_send,<N>,<total>,<avg>
```

Use for:
- Trend analysis across commits
- Performance regression detection
- Bottleneck identification
- Optimization validation

## Integration with Existing Code

### Automatic in Hot Paths

Profiling is already integrated into:
- `actor_state_machine.h` - Mailbox operations
- (More to be added as needed)

### Manual Instrumentation

Add to custom hot paths:

```c
#include "runtime/utils/aether_runtime_profile.h"

void my_hot_function(int core_id) {
    PROFILE_START();
    
    // Your code here
    do_work();
    
    PROFILE_END_ACTOR_STEP(core_id);
}
```

Available macros:
- `PROFILE_START()` - Begin timing
- `PROFILE_END_MAILBOX_SEND(core_id)`
- `PROFILE_END_MAILBOX_RECEIVE(core_id)`
- `PROFILE_END_BATCH_SEND(core_id, count)`
- `PROFILE_END_BATCH_RECEIVE(core_id, count)`
- `PROFILE_END_SPSC_ENQUEUE(core_id)`
- `PROFILE_END_SPSC_DEQUEUE(core_id)`
- `PROFILE_END_QUEUE_ENQUEUE(core_id)`
- `PROFILE_END_QUEUE_DEQUEUE(core_id)`
- `PROFILE_END_ACTOR_STEP(core_id)`
- `PROFILE_ATOMIC_OP(core_id)`
- `PROFILE_LOCK_CONTENTION(core_id)`

## Use Cases

### 1. Validate Optimizations

```bash
# Baseline
gcc -O2 -DAETHER_PROFILE -o bench_before
./bench_before > before.txt

# Apply optimization
# ... make changes ...

# Measure improvement
gcc -O2 -DAETHER_PROFILE -o bench_after
./bench_after > after.txt

# Compare
diff before.txt after.txt
```

### 2. Find Bottlenecks

Look for operations with the highest cycle counts and compare them across runs:

- `Actor Step` dominating the report is the load signal, investigate handler code.
- `SPSC Queue` cycles indicate same-core fast-path health; a sudden rise hints at queue-full or cache-line contention.
- `Mailbox` cycles are the cross-core baseline; use it as the reference for relative changes.

### 3. Monitor Production

Build your staging binary with profiling enabled to catch regressions before
production. Profiling is a **compile-time** switch (`-DAETHER_PROFILE`), not a
runtime setting, the `AETHER_PROFILE` *environment variable* is unrelated (it
selects a runtime memory profile and does **not** enable profiling output).

```bash
# Staging binary, built with profiling instrumentation
gcc -O2 -march=native -DAETHER_PROFILE your_app.c -o staging_app
./run_staging_tests.sh   # exercises the instrumented staging_app

# Check for regressions against a saved baseline. Pick TOLERANCE to
# match your run-to-run noise floor; CV% in the benchmark suite is a
# good starting point.
TOLERANCE=${TOLERANCE:-1.10}
BASELINE=$(awk '/Avg Cycles/{print $NF}' baseline.txt)
CURRENT=$(awk '/Avg Cycles/{print $NF}' profile.txt)
if awk "BEGIN { exit !($CURRENT > $BASELINE * $TOLERANCE) }"; then
    echo "Performance regression detected: current exceeds baseline × TOLERANCE"
    exit 1
fi
```

## Technical Details

### RDTSC Cycle Counting

Uses `__rdtsc()` intrinsic for cycle-accurate measurements:
- Resolution: 1 CPU cycle
- Overhead: small, fixed per measurement, consult the Intel/AMD manual for your target
- Accuracy: good for relative comparison on the same host; absolute cycle-to-wall-time conversion depends on TSC frequency

### Atomic Statistics

Stats use relaxed atomics for minimal overhead:
- Per-core stats (reduces contention)
- Atomic fetch-add for counters
- Final aggregation at report time

### Overhead Analysis

With profiling enabled:
- `__rdtsc()` instrumentation adds a small fixed overhead per measured operation. The exact cost is target-dependent; expect the instrumented path to run measurably slower than the bare one, but not so much that relative comparisons stop being meaningful.
- Acceptable for development/CI.
- **Zero overhead in production** (macros compile out).

## Examples

See:
- `tests/runtime/bench_profiled.c` - Basic example
- `tests/runtime/bench_atomic_overhead.c` - Atomic operation analysis
- `tests/runtime/micro_profile.h` - Low-level utilities

## Best Practices

1. **Always compare before/after** - Don't guess, measure
2. **Use CSV exports** - Track trends over time
3. **Profile in CI** - Catch regressions early
4. **Disable in production** - Zero overhead by default
5. **Focus on hot paths** - Profile what matters

## Optimization Validation

Using this profiling system, you can measure:
- Atomic operation overhead in hot paths
- Mailbox operation cycles
- Message copy overhead
- Queue operation latency

Profile your code to identify bottlenecks and validate optimizations on your target hardware. Results vary by platform and workload.
