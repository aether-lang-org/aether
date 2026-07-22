#!/bin/bash
# Baseline benchmark for thread-per-connection HTTP server
# Run from aether/ directory: bash benchmarks/http/run_baseline.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT_DIR"

cpu_model() {
    if command -v lscpu >/dev/null 2>&1; then
        lscpu | grep 'Model name' | sed 's/.*: *//'
    elif [ "$(uname -s)" = "Darwin" ]; then
        sysctl -n machdep.cpu.brand_string
    else
        uname -m
    fi
}

cpu_cores() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    else
        sysctl -n hw.ncpu 2>/dev/null || echo "unknown"
    fi
}

echo "=== Building baseline benchmark ==="
# Link the precompiled stdlib archive rather than naming individual
# sources. A hand-maintained source list silently rots as the runtime
# grows; `make stdlib` is the authoritative set.
make stdlib >/dev/null
gcc -O2 -Icompiler -Iruntime -Iruntime/actors -Iruntime/scheduler -Iruntime/utils \
    -Iruntime/memory -Iruntime/config -Istd -Istd/string -Istd/io -Istd/math \
    -Istd/net -Istd/collections -Istd/json \
    benchmarks/http/bench_thread_http.c build/libaether.a \
    -o build/bench_thread_http \
    -pthread -lm \
    $(pkg-config --libs openssl 2>/dev/null) \
    $(pkg-config --libs zlib 2>/dev/null) \
    $(pkg-config --libs libnghttp2 2>/dev/null) \
    $(pkg-config --libs libpcre2-8 2>/dev/null)
echo "Build OK"

RESULTS_FILE="benchmarks/http/baseline_results.txt"
echo "=== HTTP Baseline Benchmark (thread-per-connection) ===" > "$RESULTS_FILE"
echo "Date: $(date)" >> "$RESULTS_FILE"
echo "CPU: $(cpu_model)" >> "$RESULTS_FILE"
echo "Cores: $(cpu_cores)" >> "$RESULTS_FILE"
echo "" >> "$RESULTS_FILE"

# Start server in background
./build/bench_thread_http &
SERVER_PID=$!
sleep 1

# Verify server is running
if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo "ERROR: Server failed to start"
    exit 1
fi

echo "=== Running benchmarks ==="
for conns in 10 100 500 1000; do
    echo ""
    echo "--- $conns concurrent connections ---"
    echo "--- $conns concurrent connections ---" >> "$RESULTS_FILE"
    wrk -t4 -c$conns -d10s http://localhost:8080/api/hello 2>&1 | tee -a "$RESULTS_FILE"
    echo "" >> "$RESULTS_FILE"
    sleep 2
done

# Cleanup
kill $SERVER_PID 2>/dev/null
wait $SERVER_PID 2>/dev/null || true

echo ""
echo "=== Results saved to $RESULTS_FILE ==="
cat "$RESULTS_FILE"
